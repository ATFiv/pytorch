#pragma once
#include <torch/csrc/autograd/variable.h>
#include <torch/csrc/autograd/generated/variable_factories.h>
#include <torch/csrc/jit/argument_spec.h>
#include <c10/util/Exception.h>
#include <torch/csrc/jit/graph_executor.h>
#include <torch/csrc/jit/ir.h>
#include <torch/csrc/jit/named_value.h>
#include <torch/csrc/jit/passes/shape_analysis.h>
#include <torch/csrc/jit/source_range.h>

#include <torch/csrc/WindowsTorchApiMacro.h>
#include <torch/csrc/api/include/torch/ordered_dict.h>
#include <torch/csrc/utils/memory.h>

#include <ATen/core/function_schema.h>
#include <c10/util/ArrayRef.h>
#include <c10/util/Optional.h>

#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

// This file contains classes which assist in desugaring Python style
// modules and their methods into flattened graphs which don't have any
// function calls.

namespace torch {
namespace jit {
namespace script {

using ::c10::Argument;
using ::c10::FunctionSchema;
// Map which stores filename to content.
using ExtraFilesMap = std::unordered_map<std::string, std::string>;

// A method in a module, e.g. f in:
//
// class M(ScriptModule):
//   @script_method
//   def f(self, x):
//     ...
// Note: because Method/Module are exposed to python these
// classes use python method naming conventions

struct Module;

using ModuleLookup = std::function<std::shared_ptr<Module>(
    const std::vector<std::string>&)>;

struct Method {
  Method(
      Module* owner,
      std::string name,
      bool optimize,
      std::shared_ptr<Graph> graph,
      std::vector<IValue*> initial_members,
      std::function<void(Method&)> method_creator)
      : owner_(owner),
        name_(std::move(name)),
        graph_(std::move(graph)),
        optimize(optimize),
        initial_ivalues_(std::move(initial_members)),
        method_creator(std::move(method_creator)) {
    AT_ASSERT(graph_->inputs().size() >= initial_ivalues_.size());
    int i = graph_->inputs().size() - initial_ivalues_.size();
    for (auto member : initial_ivalues_) {
      initial_ivalue_index[member] = i++;
    }
  }

  void run(Stack& stack) {
    for (auto input : initial_ivalues_) {
      push(stack, *input);
    }
    get_executor().run(stack);
  }

  void run(Stack&& stack) {
    run(stack);
  }

  IValue operator()(std::vector<IValue> stack) {
    checkInputsAgainstSchema(stack);
    run(stack);
    return stack.front();
  }

  std::shared_ptr<Graph> graph_for(Stack inputs) {
    for (auto tp : initial_ivalues_) {
      inputs.emplace_back(*tp);
    }
    return get_executor().graphFor(inputs);
  }
  TORCH_API std::shared_ptr<Graph> graph() const {
    return graph_;
  }

  TORCH_API const std::string& name() const {
    return name_;
  }
  // emit a function call by inlining the callees Graph into this one
  // adding any extra parameters necessary to do this call

  // defined here to keep details of member_input handling confined to this
  // class
  Value* emit_call_to(
      const SourceRange& loc,
      Method& callee,
      ArrayRef<NamedValue> args,
      ArrayRef<NamedValue> kwargs);

  // if this isn't yet defined, run its method_creator function
  TORCH_API void ensure_defined();

  size_t num_inputs() const {
    return graph()->inputs().size() - initial_ivalues_.size();
  }
  TORCH_API Value* get_or_add_parameter(IValue* slot) {
    AT_ASSERT(slot->isTensor());
    return get_or_add_attribute(TensorType::get(), slot);
  }

  TORCH_API Value* get_or_add_attribute(TypePtr type, IValue* slot) {
    auto it = initial_ivalue_index.find(slot);
    if (it != initial_ivalue_index.end()) {
      return graph()->inputs().at(it->second);
    }
    initial_ivalues_.push_back(slot);
    initial_ivalue_index[slot] = graph()->inputs().size();
    return graph()->addInput()->setType(type);
  }

  std::shared_ptr<Graph> propagate_shapes(
      std::vector<at::Tensor> inputs,
      bool with_grad = false) {
    auto retval = graph_->copy();
    Stack stack;
    stack.reserve(inputs.size() + initial_ivalues_.size());
    for (at::Tensor& i : inputs) {
      stack.emplace_back(std::move(i));
    }
    for (IValue* inp : initial_ivalues_) {
      stack.push_back(*inp);
    }
    const auto size = stack.size();
    setInputTypes(*retval, ArgumentSpec(with_grad, stack, size));
    PropagateInputShapes(retval);
    return retval;
  }

  std::shared_ptr<Graph> propagate_and_assign_input_and_output_shapes(
      std::vector<at::Tensor> inputs,
      std::vector<at::Tensor> outputs,
      bool with_grad = false,
      bool propagate = true) {
    auto retval = graph_->copy();
    for (auto inp : initial_ivalues_) {
      if (inp->isTensor()) {
        inputs.push_back(inp->toTensor());
      }
    }
    if (propagate) {
      setInputTypes(
          *retval,
          ArgumentSpec(with_grad, fmap<IValue>(inputs), inputs.size()));
      PropagateInputShapes(retval);
    }
    AT_ASSERT(retval->inputs().size() == inputs.size());
    for (size_t i = 0; i < retval->inputs().size(); ++i) {
      auto scalar_type = inputs[i].scalar_type();
      auto sizes = inputs[i].sizes();
      auto type =
          torch::jit::CompleteTensorType::create(scalar_type, at::kCPU, sizes);
      retval->inputs()[i]->setType(type);
    }
    at::ArrayRef<Value*> output_values = retval->outputs();
    // patch this to still work if we are returning a tuple of multiple values
    if (output_values.at(0)->type()->kind() == TupleType::Kind) {
      AT_ASSERT(output_values.at(0)->node()->kind() == prim::TupleConstruct);
      output_values = output_values.at(0)->node()->inputs();
    }
    AT_ASSERT(output_values.size() == outputs.size());
    for (size_t i = 0; i < retval->outputs().size(); ++i) {
      auto scalar_type = outputs[i].scalar_type();
      auto sizes = outputs[i].sizes();
      auto type =
          torch::jit::CompleteTensorType::create(scalar_type, at::kCPU, sizes);
      output_values[i]->setType(type);
    }
    return retval;
  }

  const std::vector<IValue*>& initial_ivalues() const {
    return initial_ivalues_;
  }

  Method& setSchema(FunctionSchema schema_) {
    schema = make_unique<FunctionSchema>(std::move(schema_));
    return *this;
  }

  TORCH_API const FunctionSchema& getSchema() const {
    if (schema == nullptr) {
      schema = make_unique<FunctionSchema>(defaultSchemaFor(*this));
    }
    return *schema;
  }

  std::string pretty_print_schema() const {
    AT_ASSERT(schema);
    std::stringstream ss;
    ss << *schema;
    return ss.str();
  }

  GraphExecutorState getDebugState() {
    return get_executor().getDebugState();
  }

  void debugDisableAutodiffSubgraphInlining() {
    return get_executor().debugDisableAutodiffSubgraphInlining();
  }

  bool is_optimized() const {
    return optimize;
  }

  // the module that contains this method.
  Module& owner() const {
    return *owner_;
  }

  void check_single_output() {
    AT_CHECK(
        graph()->outputs().size() == 1,
        "Method (but not graphs in general) require a single output. Use None/Tuple for 0 or 2+ outputs");
  }

 private:
  static FunctionSchema defaultSchemaFor(const Method& method) {
    std::vector<Argument> args;
    std::vector<Argument> returns;
    Graph& g = *method.graph();
    size_t num_inputs = method.num_inputs();
    for (size_t i = 0; i < num_inputs; ++i) {
      const Value* v = g.inputs().at(i);
      std::string name = v->hasUniqueName() ? v->uniqueNameBase()
                                            : ("argument_" + std::to_string(i));
      args.emplace_back(std::move(name), unshapedType(g.inputs()[i]->type()));
    }
    for (size_t i = 0; i < g.outputs().size(); ++i) {
      returns.emplace_back("", unshapedType(g.outputs()[i]->type()));
    }
    return {method.name(), std::move(args), std::move(returns)};
  }

  GraphExecutor& get_executor() {
    std::call_once(executor_init, [&] {
      check_single_output();
      executor = GraphExecutor(graph(), optimize);
    });
    return executor;
  }

  void checkInputsAgainstSchema(std::vector<IValue>& inputs) {
    const auto& schema = getSchema();
    // Do we have more inputs than the schema accepts?
    AT_CHECK(
        inputs.size() <= schema.arguments().size(),
        "Expected at most ",
        schema.arguments().size(),
        " argument(s) for operator '",
        schema.name(),
        "', but received ",
        inputs.size(),
        " argument(s). Declaration: ",
        schema);

    for (size_t pos = 0; pos < schema.arguments().size(); ++pos) {
      const auto& argument = schema.arguments()[pos];
      if (pos < inputs.size()) {
        if (!isSubvalueOf(inputs[pos], argument.type())) {
          AT_ERROR(
            "Expected value of type ",
            *argument.type(),
            " for argument '",
            argument.name(),
            "' in position ",
            pos,
            ", but instead got value of type ",
            attemptToRecoverType(inputs[pos])->str(),
            ". Declaration: ",
            schema);
        }
      } else if (argument.default_value()) {
        inputs.push_back(*argument.default_value());
      } else {
        AT_ERROR(
            schema.name(),
            "() is missing value for argument '",
            argument.name(),
            "'. Declaration: ",
            schema);
      }
    }
  }

  // Methods are uniqued onwed by a single module. This raw pointer allows
  // looking up the module.
  Module* owner_;

  std::string name_;
  std::shared_ptr<Graph> graph_; // for debugging and for inlining
  bool optimize;

  GraphExecutor executor; // for execution
  // initial_ivalues are a list of additional arguments appended to graph
  // that are inputs that come from the members of the Module or its submodules.
  // each is a pointer to a slot in the module that owns this parameter
  // parameters and submodules can only be _added_ to script Modules to ensure
  // these pointers always stay valid
  std::vector<IValue*> initial_ivalues_;

  // map from a IValue* in initial_ivalues to the offset it appears at
  // in graph. used to accelerate get_or_add_parameter
  std::unordered_map<IValue*, size_t> initial_ivalue_index;

  // TODO: support that case where we allow _writes_ to parameters from
  // compiled functions.
  // This requires more sophisticated tracking of ssa values in Graphs so that
  // stores to all modules can be lifted to the end of a graph execution.
  // It also adds more complexity to adding actual module invocations
  // to the executor, so currently it is not done.
  // std::vector<at::Tensor*> member_outputs;

  std::once_flag executor_init;

  // an optional function that actually creates the method when
  // emit_call_to(this,...) is first called. this is used by the compiler so
  // that it can construct methods out of order
  std::function<void(Method&)> method_creator;

  // if absent, then we generate a default schema based on the graph
  // mutable because getSchema caches the default schema if one is requested
  // before a call to setSchema
  mutable std::unique_ptr<FunctionSchema> schema;
};

struct Module;

struct NamedModule {
  std::string name;
  std::shared_ptr<Module> module;
};

struct NamedIValue {
  NamedIValue(std::string name, TypePtr type, IValue ivalue)
      : name_(name),
        type(type),
        ivalue(torch::make_unique<IValue>(std::move(ivalue))) {}

  IValue* slot() const {
    return ivalue.get();
  }
  const std::string name_;
  const TypePtr type;
  std::unique_ptr<IValue> ivalue;
};

struct Module {
  TH_DISALLOW_COPY_AND_ASSIGN(Module);
  Module()
      : modules("Module"),
        parameters("Parameter"),
        attributes("Attributes"),
        methods("Method"),
        optimize(true) {}

  // note this doesn't change the flags of existing methods just ones
  // added afterward.
  void set_optimized(bool o) {
    optimize = o;
  }

  bool is_optimized() const {
    return optimize;
  }

  IValue forward(std::vector<IValue> inputs) {
    return get_method("forward")(std::move(inputs));
  }

  void register_buffer(const std::string& name, autograd::Variable v) {
    if (auto b = attributes.find(name)) {
      AT_ASSERT(b->type->isSubtypeOf(TensorType::get()));
      *b->slot() = v;
      return;
    }
    attributes.insert(name, NamedIValue(name, TensorType::get(), std::move(v)));
  }
  void register_parameter(
      const std::string& name,
      autograd::Variable v,
      bool is_buffer) {
    if (is_buffer) {
      register_buffer(name, std::move(v));
      return;
    }
    if (auto p = parameters.find(name)) {
      *p->slot() = v;
      return;
    }
    parameters.insert(name, NamedIValue(name, TensorType::get(), std::move(v)));
  }
  void register_attribute(
      const std::string& name,
      const TypePtr type,
      IValue ivalue) {
    attributes.insert(name, NamedIValue(name, type, ivalue));
  }
  void register_module(
      const std::string& name,
      std::shared_ptr<Module> module) {
    modules.insert(name, {name, std::move(module)});
  }

  Method& create_method(
      const std::string& name,
      std::shared_ptr<Graph> graph,
      std::vector<IValue*> member_inputs) {
    AT_ASSERT(graph);
    std::unique_ptr<Method> method(new Method(
        this,
        name,
        optimize,
        std::move(graph),
        std::move(member_inputs),
        nullptr));
    return *methods.insert(name, std::move(method));
  }

  Method& create_method(
      const std::string& name,
      std::function<void(Method&)> creator) {
    std::unique_ptr<Method> method(new Method(
        this,
        name,
        optimize,
        std::make_shared<Graph>(),
        {},
        std::move(creator)));
    return *methods.insert(name, std::move(method));
  }

  IValue* parameter_slot(const std::string& name) const {
    return parameters[name].slot();
  }

  void set_parameter(const std::string& name, at::Tensor v) {
    *parameter_slot(name) = std::move(v);
  }

  autograd::Variable get_parameter(const std::string& name) const {
    return autograd::as_variable_ref(parameter_slot(name)->toTensor());
  }
  autograd::Variable get_buffer(const std::string& name) const {
    return autograd::as_variable_ref(attributes.find(name)->slot()->toTensor());
  }

  // each module owns its method. The reference returned here
  // is guarenteed to stay valid until this module has been destroyed
  Method& get_method(const std::string& name) const {
    return *methods[name];
  }

  std::shared_ptr<Module> get_module(const std::string& name) const {
    return modules[name].module;
  }

  const torch::OrderedDict<std::string, NamedModule>& get_modules() const {
    return modules;
  }
  const torch::OrderedDict<std::string, NamedIValue>& get_parameters()
      const {
    return parameters;
  }
  const torch::OrderedDict<std::string, NamedIValue>& get_attributes()
      const {
    return attributes;
  }
  const torch::OrderedDict<std::string, std::unique_ptr<Method>>& get_methods()
      const {
    return methods;
  }

  NamedIValue* find_parameter(const std::string& name) {
    return parameters.find(name);
  }
  NamedIValue* find_attribute(const std::string& name) {
    return attributes.find(name);
  }
  NamedIValue* find_buffer(const std::string& name) {
    auto b = attributes.find(name);
    if (b && b->type->isSubtypeOf(TensorType::get())) {
      return b;
    }
    return nullptr;
  }
  NamedModule* find_module(const std::string& name) {
    return modules.find(name);
  }
  Method* find_method(const std::string& name) {
    if (auto* pm = methods.find(name)) {
      return pm->get();
    }
    return nullptr;
  }
  void apply(std::function<void(Module&)> fn) {
    for (auto& submod : get_modules()) {
      submod.value().module->apply(fn);
    }
    fn(*this);
  }
  /// Enables "training" mode.
  void train(bool on = true) {
    for (auto& submod : get_modules()) {
      submod->module->train(on);
    }
    register_buffer("training", torch::tensor(on ? 1 : 0, at::kLong));
  }
  /// Calls train(false) to enable "eval" mode.
  /// Do not override this method, override `train()` instead.
  void eval() {
    train(/*on=*/false);
  }
  /// True if the module is in training mode.
  bool is_training() {
    if (auto p = find_buffer("training")) {
      return p->slot()->toTensor().item<int64_t>() == 1;
    }
    // We are in training mode by default
    return true;
  }

  /// Recursively casts all parameters to the given `dtype` and `device`.
  ///
  /// If `non_blocking` is true and the source is in pinned memory and
  /// destination is on the GPU or vice versa, the copy is performed
  /// asynchronously with respect to the host. Otherwise, the argument has no
  /// effect.
  TORCH_API void to(
      at::Device device,
      at::ScalarType dtype,
      bool non_blocking = false);

  /// Recursively casts all parameters to the given dtype.
  ///
  /// If `non_blocking` is true and the source is in pinned memory and
  /// destination is on the GPU or vice versa, the copy is performed
  /// asynchronously with respect to the host. Otherwise, the argument has no
  /// effect.
  TORCH_API void to(at::ScalarType dtype, bool non_blocking = false);

  /// Recursively moves all parameters to the given device.
  ///
  /// If `non_blocking` is true and the source is in pinned memory and
  /// destination is on the GPU or vice versa, the copy is performed
  /// asynchronously with respect to the host. Otherwise, the argument has no
  /// effect.
  TORCH_API void to(at::Device device, bool non_blocking = false);

  /// Run a method from this module.
  ///
  /// For example:
  /// @code
  ///   IValue output = module->run("relu_script", a, b);
  /// @endcode
  ///
  /// To get a compile a module from a source string, see torch::jit::compile
  ///
  /// @param method_name The name of the method to run
  /// @param args Arguments to be passed to the method
  /// @return An IValue containing the return value (or values if it is a tuple)
  /// from the method
  template <typename... Types>
  IValue run_method(const std::string& method_name, Types&&... args) {
    return get_method(method_name)({IValue(std::forward<Types>(args))...});
  }

  void save(
      std::ostream& out,
      const ExtraFilesMap& extra_files = ExtraFilesMap());

  void save(
      const std::string& filename,
      const ExtraFilesMap& extra_files = ExtraFilesMap());

  void copy_into(
      ModuleLookup module_lookup,
      // parameter_remap is needed when a parent module uses a parameter of a
      // submodule
      std::unordered_map<IValue*, IValue*>& parameter_remap,
      std::vector<std::string> names = {}) const {
    auto curr = module_lookup(names);
    for (auto& kv : parameters) {
      curr->register_parameter(
          kv.key(),
          kv.value().slot()->toTensor(),
          /*is_buffer=*/false);
      parameter_remap[kv.value().slot()] = curr->parameter_slot(kv.key());
    }
    for (auto& kv : attributes) {
      if (!kv.value().type->isSubtypeOf(TensorType::get())) {
        continue;
      }
      curr->register_buffer(
          kv.key(),
          kv.value().slot()->toTensor());
      parameter_remap[kv.value().slot()] = curr->find_buffer(kv.key())->slot();
    }
    for (auto& kv : modules) {
      names.push_back(kv.key());
      // Submodules must be translated first, otherwise parameter_remap entries
      // will not be filled in for methods of this module.
      kv.value().module->copy_into(module_lookup, parameter_remap, names);
      names.pop_back();
    }
    for (auto& kv : methods) {
      std::vector<IValue*> initial_ivalues;
      for (auto& p : kv.value()->initial_ivalues()) {
        initial_ivalues.push_back(parameter_remap.at(p));
      }
      curr->create_method(
          kv.key(), kv.value()->graph()->copy(), initial_ivalues);
    }
  }

 private:
  void to_impl(
      const c10::optional<at::Device>& device,
      const c10::optional<at::ScalarType>& dtype,
      bool non_blocking);

  // invariant: to ensure initial_ivalues of Methods stay valid,
  // it is only legal to _add_ new modules and parameters.
  // removing them will allow initial_ivalues to point to invalid parameters
  // no such restriction exists for methods
  torch::OrderedDict<std::string, NamedModule> modules;
  torch::OrderedDict<std::string, NamedIValue> parameters;
  torch::OrderedDict<std::string, NamedIValue> attributes;
  torch::OrderedDict<std::string, std::unique_ptr<Method>> methods;
  bool optimize;
};

// returns nullptr and fills in failure_messages if the callee does not
// match the functions schema
Value* try_emit_call_to(
    Graph& graph,
    const SourceRange& loc,
    Method& callee,
    c10::optional<NamedValue> self,
    ArrayRef<NamedValue> args,
    ArrayRef<NamedValue> kwargs,
    std::stringstream& failure_messages,
    // when callee uses no parameters (e.g. it is a function in a compilation
    // unit, and not a method), then nullptr can be passed as caller.
    Method* caller,
    bool conv_tensors_to_nums);
} // namespace script
} // namespace jit
} // namespace torch
