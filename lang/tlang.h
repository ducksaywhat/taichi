#include <taichi/common/util.h>
#include <taichi/io/io.h>
#include <dlfcn.h>
#include <set>

TC_NAMESPACE_BEGIN

namespace Tlang {

template <typename T>
using Handle = std::shared_ptr<T>;

struct Address {
  int64 stream_id;
  int64 coeff_i;
  int64 coeff_imax;
  int64 coeff_const;  // offset

  // AOSOA: i / a * b
  int64 coeff_aosoa_group_size;
  int64 coeff_aosoa_stride;

  TC_IO_DEF(stream_id,
            coeff_i,
            coeff_imax,
            coeff_const,
            coeff_aosoa_stride,
            coeff_aosoa_group_size);

  Address() {
    stream_id = -1;
    coeff_i = 0;
    coeff_imax = 0;
    coeff_const = 0;
    coeff_aosoa_group_size = 0;
    coeff_aosoa_stride = 0;
  }

  bool initialized() {
    return stream_id != -1;
  }

  TC_FORCE_INLINE bool same_type(Address o) {
    return stream_id == o.stream_id && coeff_i == o.coeff_i &&
           coeff_imax == o.coeff_imax &&
           coeff_aosoa_group_size == o.coeff_aosoa_group_size &&
           coeff_aosoa_stride == o.coeff_aosoa_stride;
  }

  TC_FORCE_INLINE bool operator==(Address o) {
    return stream_id == o.stream_id && coeff_i == o.coeff_i &&
           coeff_imax == o.coeff_imax && coeff_const == o.coeff_const &&
           coeff_aosoa_group_size == o.coeff_aosoa_group_size &&
           coeff_aosoa_stride == o.coeff_aosoa_group_size;
  }

  TC_FORCE_INLINE int64 offset() {
    return coeff_const;
  }

  int64 eval(int64 i, int64 n) {
    TC_ASSERT(initialized());
    if (coeff_aosoa_stride != 0) {
      return coeff_i * i + coeff_imax * n + coeff_const +
             (i / coeff_aosoa_group_size) * coeff_aosoa_stride;
    } else {
      return coeff_i * i + coeff_imax * n + coeff_const;
    }
  }
};

class Expr;

// TODO: do we need polymorphism here?
class Node {
 public:
  enum class Type : int { mul, add, sub, div, load, store, combine, constant };

  Address addr;
  std::vector<Expr> ch;       // Four child max
  std::vector<Expr> members;  // for vectorized instructions
  Type type;
  std::string var_name;
  float64 value;
  bool is_vectorized;

  Node(Type type) : type(type) {
    is_vectorized = false;
  }

  Node(Type type, Expr ch0, Expr ch1);

  int member_id(const Expr &expr) const;
};

using NodeType = Node::Type;

// Reference counted...
class Expr {
 private:
  Handle<Node> node;

 public:
  Expr() {
  }

  Expr(float64 val) {
    // cretea a constant node
    node = std::make_shared<Node>(NodeType::constant);
    node->value = val;
  }

  Expr(Handle<Node> node) : node(node) {
  }

  template <typename... Args>
  static Expr create(Args &&... args) {
    return Expr(std::make_shared<Node>(std::forward<Args>(args)...));
  }

#define BINARY_OP(op, name)                            \
  Expr operator op(const Expr &o) const {              \
    return Expr::create(NodeType::name, node, o.node); \
  }

  BINARY_OP(*, mul);
  BINARY_OP(+, add);
  BINARY_OP(-, sub);
  BINARY_OP(/, div);
#undef BINARY_OP

  Expr store(const Expr &e, Address addr) {
    if (!node) {
      node = std::make_shared<Node>(NodeType::combine);
    }
    auto n = std::make_shared<Node>(NodeType::store);
    n->ch.push_back(e.node);
    n->addr = addr;
    Expr store_e(n);
    node->ch.push_back(n);
    return store_e;
  }

  Node *operator->() {
    return node.get();
  }

  const Node *operator->() const {
    return node.get();
  }

  bool operator<(const Expr &o) const {
    return node.get() < o.node.get();
  }

  operator bool() const {
    return node.get() != nullptr;
  }

  operator void *() const {
    return (void *)node.get();
  }

  bool operator==(const Expr &o) const {
    return (void *)(*this) == (void *)o;
  }
};

Node::Node(Type type, Expr ch0, Expr ch1) : Node(type) {
  ch.resize(2);
  ch[0] = ch0;
  ch[1] = ch1;
}

inline Expr load(Address addr) {
  auto n = std::make_shared<Node>(NodeType::load);
  TC_ASSERT(addr.initialized());
  n->addr = addr;
  TC_ASSERT(0 <= addr.stream_id && addr.stream_id < 3);
  return Expr(n);
}

int Node::member_id(const Expr &expr) const {
  for (int i = 0; i < members.size(); i++) {
    if (members[i] == expr) {
      return i;
    }
  }
  return -1;
}

inline int get_code_gen_id() {
  static int id = 0;
  TC_ASSERT(id < 10000);
  return id++;
}

class CodeGen {
  int var_count;
  std::string code;

 public:
  std::string func_name;
  enum class Mode : int { scalar, vector };

  Mode mode;
  int simd_width;
  int group_size;
  int num_groups;
  int id;
  std::map<NodeType, std::string> binary_ops;
  std::string folder;

  CodeGen(Mode mode = Mode::vector, int simd_width = 8)
      : var_count(0), mode(mode), simd_width(simd_width) {
  }

  std::string create_variable() {
    TC_ASSERT(var_count < 10000);
    return fmt::format("var_{:04d}", var_count++);
  }

  using FunctionType = void (*)(float32 *, float32 *, float32 *, int);

  std::string run(Expr &expr, int group_size = 1) {
    TC_ASSERT(mode == Mode::vector);
    this->group_size = group_size;
    TC_ASSERT(group_size != 0);
    // group_size = expr->ch.size();
    num_groups = simd_width / group_size;
    TC_WARN_IF(simd_width % group_size != 0, "insufficient lane usage");

    id = get_code_gen_id();
    func_name = fmt::format("func{:06d}", id);
    binary_ops[NodeType::add] = "+";
    binary_ops[NodeType::sub] = "-";
    binary_ops[NodeType::mul] = "*";
    binary_ops[NodeType::div] = "/";
    folder = "_tlang_cache/";
    create_directories(folder);
    code = "#include <immintrin.h>\n#include <cstdio>\n";
    code += "using float32 = float;\n";
    code += "using float64 = double;\n\n";
    code += "extern \"C\" void " + func_name +
            "(float32 *stream00, float32 *stream01, float32 "
            "*stream02, "
            "int n) {\n";
    code += fmt::format("for (int i = 0, g = 0; i < n; i += {}, g++) {{\n",
                        num_groups);
    auto vectorized_expr = vectorize(expr, group_size, num_groups);
    code_gen(vectorized_expr);
    code += "}\n}\n";
    return code;
  }

  std::string get_scalar_suffix(int i) {
    return fmt::format("_{:03d}", i);
  }

  std::vector<Expr> reachable_exprs(Expr &expr) {
    std::vector<Expr> ret;
    std::set<Expr> visited;

    std::function<void(Expr &)> visit = [&](Expr &expr) {
      if (visited.find(expr) != visited.end())
        return;
      visited.insert(expr);
      ret.push_back(expr);
      for (auto c : expr->ch) {
        code_gen(c);
      }
    };
    code_gen(expr);
    return ret;
  }

  Expr repeat(Expr &expr, int offset) {
    TC_NOT_IMPLEMENTED
    std::set<Expr> visited;
    Expr new_expr;

    // Copy with duplication detection

    return new_expr;
  }

  std::map<Expr, Expr> scalar_to_vector;
  // Create vectorized IR
  // the vector width should be the final SIMD instruction width
  Expr vectorize(Expr &expr, int group_size, int num_groups) {
    TC_ASSERT(group_size * num_groups == simd_width);
    scalar_to_vector.clear();
    // expr should be a ret Op, with its children store Ops.
    // The stores are repeated by a factor of 'pack_size'
    TC_ASSERT(expr->ch.size() % group_size == 0);
    TC_ASSERT(expr->type == NodeType::combine);
    // Create the root group
    auto combined = Expr::create(NodeType::combine);
    combined->is_vectorized = true;
    for (int k = 0; k < expr->ch.size() / group_size; k++) {
      auto root = Expr::create(NodeType::store);
      root->is_vectorized = true;
      bool has_prior_to = false, has_same = false;
      for (int i = k * group_size; i < (k + 1) * group_size; i++) {
        auto ch = expr->ch[i];
        TC_ASSERT(ch->type == NodeType::store);
        root->members.push_back(ch);
        if (i > k * group_size) {
          if (prior_to(root->members[i - 1], root->members[i])) {
            has_prior_to = true;
          } else if (root->members[i - 1]->addr == root->members[i]->addr) {
            has_same = true;
          } else {
            TC_P(root->members[i - 1]->addr);
            TC_P(root->members[i]->addr);
            TC_ERROR(
                "Addresses in SIMD load should be either identical or "
                "neighbouring.");
          }
        }
      }
      TC_ASSERT(!(has_prior_to && has_same));
      // TC_P(root->members.size());
      vectorize(root);
      combined->ch.push_back(root);
    }
    // TC_P(combined->ch.size());
    return combined;
  }

  void vectorize(Expr &expr) {
    // TC_P((int)expr->type);
    // Note: expr may be replaced by an existing vectorized Expr
    if (scalar_to_vector.find(expr->members[0]) != scalar_to_vector.end()) {
      auto existing = scalar_to_vector[expr->members[0]];
      TC_ASSERT(existing->members.size() == expr->members.size());
      for (int i = 0; i < existing->members.size(); i++) {
        TC_ASSERT(existing->members[i] == expr->members[i]);
      }
      expr = existing;
      return;
    }

    expr->is_vectorized = true;
    bool first = true;
    NodeType type;
    std::vector<std::vector<Expr>> vectorized_children;

    // Check for isomorphism
    for (auto member : expr->members) {
      // It must not appear to an existing vectorized expr
      TC_ASSERT(scalar_to_vector.find(member) == scalar_to_vector.end());
      if (first) {
        first = false;
        type = member->type;
        vectorized_children.resize(member->ch.size());
      } else {
        TC_ASSERT(type == member->type);
        TC_ASSERT(vectorized_children.size() == member->ch.size());
      }
      for (int i = 0; i < member->ch.size(); i++) {
        vectorized_children[i].push_back(member->ch[i]);
      }
    }

    expr->is_vectorized = true;
    TC_ASSERT(expr->members.size() % group_size == 0);

    for (int i = 0; i < vectorized_children.size(); i++) {
      // TC_P(i);
      auto ch = Expr::create(vectorized_children[i][0]->type);
      ch->members = vectorized_children[i];
      expr->ch.push_back(ch);
      vectorize(ch);
    }

    expr->addr = expr->members[0]->addr;
    if (expr->addr.coeff_aosoa_group_size == 0) {
      expr->addr.coeff_aosoa_group_size = num_groups;
      expr->addr.coeff_aosoa_stride = 0;
    }
  }

  /*
  void vectorized_codegen(Expr &expr) {
    for (auto &c: expr->ch) {
      if (c) {
        vectorized_codegen(c);
      }
    }
    if (expr->var_name == "") {
      expr->var_name = create_variable();
    } else
      return; // visited
  }
  */

  std::string get_vectorized_address(Address addr) {
    auto stream_name = fmt::format("stream{:02d}", addr.stream_id);
    auto stride = addr.coeff_i * num_groups + group_size /
                                                  addr.coeff_aosoa_group_size *
                                                  addr.coeff_aosoa_stride;
    auto offset = addr.coeff_const;
    return fmt::format("&{}[{} * n + {} * g + {}]\n", stream_name,
                       addr.coeff_imax, stride, offset);
  }

  void code_gen(Expr &expr) {
    TC_ASSERT(expr->is_vectorized);
    TC_ASSERT(expr->members.size() == 0 || expr->members.size() == group_size);
    // TC_P(expr->ch.size());
    for (auto &c : expr->ch) {
      if (c)
        code_gen(c);
    }
    if (expr->var_name == "")
      expr->var_name = create_variable();
    else
      return;  // visited
    if (binary_ops.find(expr->type) != binary_ops.end()) {
      auto op = binary_ops[expr->type];
      if (mode == Mode::vector) {
        code += fmt::format("auto {} = {} {} {};\n", expr->var_name,
                            expr->ch[0]->var_name, op, expr->ch[1]->var_name);
      } else if (mode == Mode::scalar) {
        for (int i = 0; i < simd_width; i++) {
          auto suf = get_scalar_suffix(i);
          code += fmt::format("auto {} = {} {} {};\n", expr->var_name + suf,
                              expr->ch[0]->var_name + suf, op,
                              expr->ch[1]->var_name + suf);
        }
      }
    } else if (expr->type == NodeType::load) {
      auto stream_name = fmt::format("stream{:02d}", expr->addr.stream_id);

      if (mode == Mode::vector) {
        // TC_P(expr->members.size());
        std::vector<int> offsets;
        for (int i = 0; i + 1 < (int)expr->members.size(); i++) {
          TC_ASSERT(
              expr->members[i]->addr.same_type(expr->members[i + 1]->addr));
        }
        for (int i = 0; i < (int)expr->members.size(); i++) {
          offsets.push_back(expr->members[i]->addr.offset());
        }
        auto addr = expr->addr;
        auto i_stride = num_groups;
        // TC_P(i_stride);
        // TC_P(addr.coeff_aosoa_group_size);
        TC_ASSERT(i_stride == addr.coeff_aosoa_group_size);
        // TC_ASSERT(expr->members[0]->addr.coeff_i);
        std::string load_instr =
            simd_width == 8 ? "_mm256_load_ps" : "_mm512_load_ps";
        bool needs_shuffle = false;
        if (addr.coeff_const % simd_width != 0) {
          addr.coeff_const -= addr.coeff_const % simd_width;
          needs_shuffle = true;
        }
        code += fmt::format("auto {}_immediate = {}({});\n", expr->var_name,
                            load_instr, get_vectorized_address(addr));
        auto emit_shuffle = [&](std::string imm) {
          code += fmt::format(
              "auto {} = _mm256_shuffle_ps({}_immediate, {}_immediate, {});\n",
              expr->var_name, expr->var_name, expr->var_name, imm);
          needs_shuffle = false;
        };
        if (group_size == 1) {
          code += fmt::format("auto {} = {}_immediate;\n", expr->var_name,
                              expr->var_name);
        } else {
          TC_ASSERT(group_size <= 2);
          // detect patterns
          int offset_const = offsets[0] % simd_width;
          int offset_inc = offsets[1] - offsets[0];
          if (offset_const == 0 && offset_inc == 1) {
            code += fmt::format("auto {} = {}_immediate;\n", expr->var_name,
                                expr->var_name);
          } else if (offset_inc == 0) {
            if (offset_const == 0) {
              emit_shuffle("0xA0");
            } else if (offset_const == 1) {
              emit_shuffle("0xF5");
            } else {
              TC_NOT_IMPLEMENTED;
            }
          } else {
            TC_P(offset_const);
            TC_P(offset_inc);
            TC_NOT_IMPLEMENTED;
          }
          TC_ASSERT(needs_shuffle == false);
        }

      } else {
        TC_NOT_IMPLEMENTED
        for (int i = 0; i < simd_width; i++) {
          auto suf = get_scalar_suffix(i);
          code += fmt::format("auto {} = {}[{} * i + {} + {}];\n",
                              expr->var_name + suf, stream_name,
                              expr->addr.coeff_i, expr->addr.coeff_const, i);
        }
      }
    } else if (expr->type == NodeType::store) {
      auto stream_name = fmt::format("stream{:02d}", expr->addr.stream_id);
      if (mode == Mode::vector) {
        std::string store_instr =
            simd_width == 8 ? "_mm256_store_ps" : "_mm512_store_ps";
        code += fmt::format("{}({}, {});\n", store_instr,
                            get_vectorized_address(expr->addr),
                            expr->ch[0]->var_name);
      } else {
        TC_NOT_IMPLEMENTED
        for (int i = 0; i < simd_width; i++) {
          auto suf = get_scalar_suffix(i);
          code += fmt::format("{}[{} * i + {} + {}] = {};\n", stream_name,
                              expr->addr.coeff_i, expr->addr.coeff_const, i,
                              expr->ch[0]->var_name + suf);
        }
      }
    } else if (expr->type == NodeType::combine) {
      // do nothing
    } else {
      TC_P((int)expr->type);
      TC_NOT_IMPLEMENTED;
    }
  }

  std::string get_source_fn() {
    return fmt::format("{}/tmp{:04d}.cpp", folder, id);
  }

  std::string get_library_fn() {
#if defined(TC_PLATFORM_OSX)
    // Note: use .so here will lead to wired behavior...
    return fmt::format("{}/tmp{:04d}.dylib", folder, id);
#else
    return fmt::format("{}/tmp{:04d}.so", folder, id);
#endif
  }

  FunctionType get(Expr &e, int group_size) {
    // SLP(e, group_size);
    run(e, group_size);
    {
      std::ofstream of(get_source_fn());
      of << code;
    }
    std::system(fmt::format("clang-format -i {}", get_source_fn()).c_str());
    auto cmd = fmt::format(
        "g++ {} -std=c++14 -shared -fPIC -O3 -march=native "
        "-D_GLIBCXX_USE_CXX11_ABI=0 -o {}",
        get_source_fn(), get_library_fn());
    auto compile_ret = std::system(cmd.c_str());
    TC_ASSERT(compile_ret == 0);
    /*
    system(
        fmt::format("objdump {} -d > {}.s", get_library_fn(), get_library_fn())
            .c_str());
            */
    auto dll = dlopen(("./" + get_library_fn()).c_str(), RTLD_LAZY);
    TC_ASSERT(dll != nullptr);
    auto ret = dlsym(dll, func_name.c_str());
    TC_ASSERT(ret != nullptr);
    return (FunctionType)ret;
  }

  bool prior_to(Expr &a, Expr &b) {
    auto address1 = a->addr;
    auto address2 = b->addr;
    return address1.same_type(address2) &&
           address1.offset() + 1 == address2.offset();
  }

  std::vector<Expr> extract_instructions(Expr root_expr) {
    std::vector<Expr> inst;
    std::set<void *> visited;

    std::function<void(Expr)> walk = [&](Expr expr) -> void {
      TC_ASSERT(expr);
      if (visited.find(expr) != visited.end())
        return;
      visited.insert(expr);
      for (auto &ch : expr->ch) {
        walk(ch);
      }
      inst.push_back(expr);
    };

    walk(root_expr);

    return inst;
  }

  std::vector<Expr> inst;
  std::vector<std::vector<int>> groups;
  std::vector<bool> grouped;

  std::vector<int> continuous_loads(int i) {
    std::vector<int> ret;
    if (grouped[i] || inst[i]->type != NodeType::load) {
      return ret;
    }
    ret.push_back(i);
    while (1) {
      bool found = false;
      for (int j = 0; j < inst.size(); j++) {
        if (grouped[j] || i == j || inst[i]->type != NodeType::load) {
          continue;
        }
        if (prior_to(inst[i], inst[j])) {
          ret.push_back(j);
          i = j;
          found = true;
          break;
        }
      }
      if (!found) {
        break;
      }
    }
    return ret;
  }

  void SLP(Expr expr, int group_size) {
    inst = extract_instructions(expr);
    TC_INFO("# instructions = {}", inst.size());
    grouped = std::vector<bool>(inst.size(), false);

    while (true) {
      // Enumerate the instructions
      int maximum_length = 0;
      int inst_with_max_length = -1;
      std::vector<int> C;
      for (int i = 0; i < inst.size(); i++) {
        auto c = continuous_loads(i);
        if (c.size() > maximum_length) {
          maximum_length = c.size();
          inst_with_max_length = i;
          C = c;
        }
      }

      // Extend
      if (inst_with_max_length != -1) {
        TC_P(C);
        // Pack
        TC_WARN_IF(C.size() % group_size != 0, "C.size() = {}", C.size());
        groups.push_back(std::vector<int>());
        for (int i = 0; i < C.size(); i++) {
          grouped[C[i]] = true;
          groups.back().push_back(C[i]);
        }
      } else {
        break;
      }
    }

    TC_INFO("# groups {}", groups.size());
    for (int i = 0; i < groups.size(); i++) {
      TC_INFO("Group {} size = {}", i, groups[i].size());
    }

    /*
    while (true) {
      propagate();
    }
    */
  }
};
}  // namespace Tlang

TC_NAMESPACE_END

/*
 Expr should be what the users play with.
   Simply a ref-counted pointer to nodes, with some operator overloading for
 users to program Node is the IR node, with computational graph connectivity,
 imm, op type etc.

 No double support this time.
 */
