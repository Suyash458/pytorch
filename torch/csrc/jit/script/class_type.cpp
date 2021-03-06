#include <ATen/core/jit_type.h>
#include <c10/macros/Macros.h>
#include <torch/csrc/jit/script/module.h>

namespace c10 {

ClassTypePtr ClassType::create(
    c10::optional<QualifiedName> qualifiedName,
    std::weak_ptr<CompilationUnit> cu,
    bool is_module) {
  return ClassTypePtr(
      new ClassType(std::move(qualifiedName), std::move(cu), is_module));
}

ClassType::ClassType(
    c10::optional<QualifiedName> name,
    std::weak_ptr<CompilationUnit> cu,
    bool is_module)
    : NamedType(TypeKind::ClassType, std::move(name)),
      compilation_unit_(std::move(cu)) {
  if (is_module) {
    parameterSlots_ = std::make_shared<std::vector<bool>>();
  }
}

const std::vector<Function*>& ClassType::methods() const {
  return methods_;
}

void ClassType::checkNotExist(const std::string& name, const std::string& what)
    const {
  // Check no overlap with existing constants
  for (size_t i = 0; i < constantNames_.size(); ++i) {
    TORCH_CHECK(
        name != constantNames_[i],
        "attempting to add ",
        what,
        " '",
        name,
        "' to ",
        python_str(),
        " but a constant field of the same name already exists with value ",
        constantValues_[i]);
  }

  // Check no overlap with existing attributes
  for (size_t i = 0; i < attributeNames_.size(); ++i) {
    TORCH_CHECK(
        name != attributeNames_[i],
        "attempting to add ",
        what,
        " '",
        name,
        "' to ",
        python_str(),
        " but an attribute field of the same name already exists with type ",
        attributeTypes_[i]->python_str());
  }
}

size_t ClassType::addAttribute(
    const std::string& name,
    const TypePtr& type,
    bool is_parameter) {
  const char* what = is_parameter ? "parameter" : "attribute";
  checkNotExist(name, what);
  checkNoAny(*this, what, name, type);

  size_t slot = attributeNames_.size();
  attributeNames_.push_back(name);
  attributeTypes_.push_back(type);
  if (is_parameter) {
    TORCH_INTERNAL_ASSERT(is_module(), "adding a parameter to a non module");
    TORCH_CHECK(
        (type->kind() == TensorType::Kind) ||
            (type->kind() == OptionalType::Kind &&
             type->expect<OptionalType>()->getElementType()->kind() ==
                 TensorType::Kind) ||
            (type->kind() == NoneType::Kind),
        "Expecting parameter to have either None, Tensor or Optional[Tensor] type, but got: ",
        toString(type));
  }
  if (is_module()) {
    parameterSlots_->push_back(is_parameter);
  }
  return slot;
}

void ClassType::unsafeRemoveAttribute(const std::string& name) {
  auto slot = getAttributeSlot(name);
  attributeNames_.erase(attributeNames_.begin() + slot);
  attributeTypes_.erase(attributeTypes_.begin() + slot);
  if (is_module()) {
    parameterSlots_->erase(parameterSlots_->begin() + slot);
  }
}

void ClassType::addMethod(Function* method) {
  TORCH_CHECK(
      getMethod(method->name()) == nullptr,
      "Can't redefine method: ",
      method->name(),
      " on class: ",
      python_str());
  methods_.push_back(method);
}

Function* ClassType::getMethod(const std::string& name) const {
  for (auto method : methods_) {
    if (name == method->name()) {
      return method;
    }
  }
  return nullptr;
}

size_t ClassType::addConstant(const std::string& name, const IValue& value) {
  checkNotExist(name, "constant");
  size_t slot = constantNames_.size();
  constantNames_.push_back(name);
  constantValues_.push_back(value);
  return slot;
}

IValue ClassType::getConstant(const std::string& name) const {
  const auto& v = findConstant(name);
  TORCH_CHECK(
      v.has_value(),
      python_str(),
      " does not have a constant field with name '",
      name,
      "'");
  return *v;
}

IValue ClassType::getConstant(size_t slot) const {
  TORCH_INTERNAL_ASSERT(constantNames_.size() == constantValues_.size());
  TORCH_CHECK(
      slot < constantValues_.size(),
      python_str(),
      " does not have a constant slot of index ",
      slot);
  return constantValues_[slot];
}

c10::optional<IValue> ClassType::findConstant(const std::string& name) const {
  TORCH_INTERNAL_ASSERT(constantNames_.size() == constantValues_.size());
  size_t pos = 0;
  for (const auto& c : constantNames_) {
    if (name == c) {
      break;
    }
    ++pos;
  }

  if (pos >= constantNames_.size()) {
    return c10::nullopt;
  }
  return constantValues_[pos];
}

void ClassType::unsafeRemoveConstant(const std::string& name) {
  auto slot = getConstantSlot(name);
  constantNames_.erase(constantNames_.begin() + slot);
  constantValues_.erase(constantValues_.begin() + slot);
}

std::shared_ptr<CompilationUnit> ClassType::compilation_unit() {
  auto cu = compilation_unit_.lock();
  TORCH_INTERNAL_ASSERT(cu);
  return cu;
}
std::shared_ptr<const CompilationUnit> ClassType::compilation_unit() const {
  auto cu = compilation_unit_.lock();
  TORCH_INTERNAL_ASSERT(cu);
  return cu;
}

#ifndef USE_MOBILE_CLASSTYPE

// This file exists because we need to reference module.h, which we can't from
// c10. Sigh...
FunctionType::FunctionType(Function* function)
    : NamedType(TypeKind::FunctionType, function->qualname()),
      function_(function) {}

ClassTypePtr ClassType::refine(at::ArrayRef<TypePtr> refined_slots) const {
  auto ptr = ClassType::create(name(), compilation_unit_);
  AT_ASSERT(numAttributes() == refined_slots.size());
  for (size_t i = 0; i < attributeNames_.size(); ++i) {
    AT_ASSERT(refined_slots[i]->isSubtypeOf(attributeTypes_[i]));
    ptr->addAttribute(attributeNames_[i], refined_slots[i]);
  }
  // Copy methods over
  for (const auto& method : methods()) {
    ptr->addMethod(method);
  }
  return ptr;
}

bool ClassType::isSubtypeOfExt(const TypePtr rhs, std::ostream* why_not) const {
  // to improve performance, this check can be cached
  if (auto iface = rhs->cast<InterfaceType>()) {
    // ClassType is not a subtype of InterfaceType if the InterfaceType is a
    // Module Interface Type but the Class Type is not a Module Class Type
    if (!is_module() && iface->is_module()) {
      if (why_not) {
        *why_not << "Class '" << python_str() << "' is not a subtype of "
                 << "the module interface '" << rhs->python_str()
                 << "' , only ScriptModule class can be subtype of module"
                 << " interface.\n";
      }
      return false;
    }
    for (const FunctionSchema& schema : iface->methods()) {
      auto self_method = getMethod(schema.name());
      if (!self_method) {
        if (why_not) {
          *why_not << "Class '" << python_str() << "' does not have method '"
                   << schema.name() << "' but '" << rhs->python_str()
                   << "' does.\n";
        }
        return false;
      }
      if (!self_method->getSchema().isSubtypeOf(
              schema, /*is_method=*/true, why_not)) {
        if (why_not) {
          *why_not << "Method on class '" << python_str()
                   << "' (1) is not compatible with interface '"
                   << rhs->python_str() << "' (2)\n"
                   << "  (1) " << self_method->getSchema() << "\n"
                   << "  (2) " << schema << "\n";
        }
        return false;
      }
    }
    return true;
  }
  return Type::isSubtypeOfExt(rhs, why_not);
}
#else
bool ClassType::isSubtypeOfExt(const TypePtr rhs, std::ostream* why_not) const {
  return Type::isSubtypeOfExt(rhs, why_not);
}
#endif
} // namespace c10
