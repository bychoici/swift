//===--- SILGenLValue.cpp - Constructs logical lvalues for SILGen ---------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SILGen.h"
#include "swift/AST/AST.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "LValue.h"
#include "RValue.h"
#include "swift/SIL/TypeLowering.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;
using namespace Lowering;

void PathComponent::_anchor() {}
void PhysicalPathComponent::_anchor() {}
void LogicalPathComponent::_anchor() {}

namespace {
  class AddressComponent : public PhysicalPathComponent {
    SILValue address;
  public:
    AddressComponent(SILValue address) : address(address) {
      assert(address.getType().isAddress() &&
             "var component value must be an address");
    }
    
    SILValue offset(SILGenFunction &gen, SILLocation loc,
                 SILValue base) const override {
      assert(!base && "var component must be root of lvalue path");
      return address;
    }
    
    Type getObjectType() const override {
      return address.getType().getSwiftRValueType();
    }
  };
  
  class RefElementComponent : public PhysicalPathComponent {
    VarDecl *field;
    SILType type;
  public:
    RefElementComponent(VarDecl *field, SILType type)
      : field(field), type(type) {}
    
    SILValue offset(SILGenFunction &gen, SILLocation loc, SILValue base)
      const override
    {
      assert(!base.getType().isAddress() &&
             "base for ref element component can't be an address");
      assert(base.getType().hasReferenceSemantics() &&
             "base for ref element component must be a reference type");
      return gen.B.createRefElementAddr(loc, base, field, type);
    }
    
    Type getObjectType() const override {
      return type.getSwiftRValueType();
    }
  };
  
  class FragileElementComponent : public PhysicalPathComponent {
    unsigned elementIndex;
    SILType type;
  public:
    FragileElementComponent(unsigned elementIndex, SILType type)
      : elementIndex(elementIndex), type(type) {}
    
    SILValue offset(SILGenFunction &gen, SILLocation loc,
                 SILValue base) const override {
      assert(base && "invalid value for element base");
      SILType baseType = base.getType();
      (void)baseType;
      assert(baseType.isAddress() &&
             "base for element component must be an address");
      assert(!baseType.hasReferenceSemantics() &&
             "can't get element from address of ref type");
      return gen.B.createElementAddr(loc, base, elementIndex, type);
    }
    
    Type getObjectType() const override {
      return type.getSwiftRValueType();
    }
  };

  class RefComponent : public PhysicalPathComponent {
    SILValue value;
  public:
    RefComponent(ManagedValue value) : value(value.getValue()) {
      assert(value.getType().hasReferenceSemantics() &&
             "ref component must be of reference type");
    }
    
    SILValue offset(SILGenFunction &gen, SILLocation loc,
                 SILValue base) const override {
      assert(!base && "ref component must be root of lvalue path");
      return value;
    }

    Type getObjectType() const override {
      return value.getType().getSwiftRValueType();
    }
  };
  
  class GetterSetterComponent : public LogicalPathComponent {
    SILConstant getter;
    SILConstant setter;
    std::vector<Substitution> substitutions;
    Expr *subscriptExpr;
    Type substType;
    
    struct AccessorArgs {
      RValue base;
      RValue subscripts;
    };
    
    /// Returns a tuple of RValues holding the accessor value, base (retained if
    /// necessary), and subscript arguments, in that order.
    AccessorArgs
    prepareAccessorArgs(SILGenFunction &gen,
                        SILLocation loc,
                        SILValue base) const
    {
      assert((!base || (base.getType().isAddress() ^
                        base.getType().hasReferenceSemantics())) &&
             "base of getter/setter component must be invalid, lvalue, or "
             "of reference type");
      
      AccessorArgs result;      
      if (base) {
        if (base.getType().hasReferenceSemantics()) {
          gen.B.createRetain(loc, base);
          result.base = RValue(gen, gen.emitManagedRValueWithCleanup(base));
        } else {
          result.base = RValue(gen, ManagedValue(base, ManagedValue::LValue));
        }
      }
      
      if (subscriptExpr)
        result.subscripts = gen.visit(subscriptExpr);
      
      return result;
    }
    
  public:
    GetterSetterComponent(SILConstant getter, SILConstant setter,
                          ArrayRef<Substitution> substitutions,
                          Type substType)
      : getter(getter), setter(setter), substitutions(substitutions.begin(),
                                                      substitutions.end()),
        subscriptExpr(nullptr),
        substType(substType)
    {
      assert(!getter.isNull() && !setter.isNull() &&
             "settable lvalue must have both getter and setter");
    }

    GetterSetterComponent(SILConstant getter, SILConstant setter,
                          ArrayRef<Substitution> substitutions,
                          Expr *subscriptExpr,
                          Type substType)
      : getter(getter), setter(setter), substitutions(substitutions.begin(),
                                                      substitutions.end()),
        subscriptExpr(subscriptExpr),
        substType(substType)
    {
      assert(!getter.isNull() && !setter.isNull() &&
             "settable lvalue must have both getter and setter");
    }
    
    void storeRValue(SILGenFunction &gen, SILLocation loc,
                     RValue &&rvalue, SILValue base) const override
    {
      auto args
        = prepareAccessorArgs(gen, loc, base);
      
      return gen.emitSetProperty(loc,
                                 setter, substitutions,
                                 std::move(args.base),
                                 std::move(args.subscripts),
                                 std::move(rvalue));
    }
    
    Materialize loadAndMaterialize(SILGenFunction &gen, SILLocation loc,
                                   SILValue base) const override
    {
      auto args
        = prepareAccessorArgs(gen, loc, base);
      
      return gen.emitGetProperty(loc,
                                 getter, substitutions,
                                 std::move(args.base),
                                 std::move(args.subscripts),
                                 substType);
    }
    
    Type getObjectType() const override {
      return substType;
    }
  };
}

LValue SILGenLValue::visitRec(Expr *e) {
  if (e->getType()->hasReferenceSemantics()) {
    // Any reference type expression can form the root of a logical lvalue.
    LValue lv;
    lv.add<RefComponent>(gen.visit(e).getAsSingleValue(gen));
    return ::std::move(lv);
  } else {
    return visit(e);
  }
}

LValue SILGenLValue::visitExpr(Expr *e) {
  e->dump();
  llvm_unreachable("unimplemented lvalue expr");
}

LValue SILGenLValue::visitDeclRefExpr(DeclRefExpr *e) {
  LValue lv;
  ValueDecl *decl = e->getDecl();

  // If it's a property, push a reference to the getter and setter.
  if (VarDecl *var = dyn_cast<VarDecl>(decl)) {
    if (var->isProperty()) {
      lv.add<GetterSetterComponent>(SILConstant(var, SILConstant::Kind::Getter),
                                    SILConstant(var, SILConstant::Kind::Setter),
                                    ArrayRef<Substitution>{},
                                    e->getType()->getRValueType());
      return ::std::move(lv);
    }
  }

  // If it's a physical value, push its address.
  SILValue address = gen.emitReferenceToDecl(e, decl).getUnmanagedValue();
  assert(address.getType().isAddress() &&
         "physical lvalue decl ref must evaluate to an address");
  lv.add<AddressComponent>(address);
  return ::std::move(lv);
}

LValue SILGenLValue::visitMaterializeExpr(MaterializeExpr *e) {
  LValue lv;
  SILValue materialized = gen.visit(e).getUnmanagedSingleValue(gen);
  lv.add<AddressComponent>(materialized);
  return ::std::move(lv);
}

LValue SILGenLValue::visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *e)
{
  gen.visit(e->getLHS());
  return visitRec(e->getRHS());
}

namespace {
  
template<typename ANY_MEMBER_REF_EXPR>
LValue emitAnyMemberRefExpr(SILGenLValue &sgl,
                            SILGenFunction &gen,
                            ANY_MEMBER_REF_EXPR *e,
                            ArrayRef<Substitution> substitutions) {
  LValue lv = sgl.visitRec(e->getBase());
  ValueDecl *decl = e->getDecl();
  SILType baseTy = gen.getLoweredType(e->getBase()->getType()->getRValueType());

  // If this is a physical field, access with a fragile element reference.
  if (VarDecl *var = dyn_cast<VarDecl>(decl)) {
    if (!var->isProperty()) {
      if (baseTy.hasReferenceSemantics()) {
        lv.add<RefElementComponent>(var,
                                    gen.getLoweredType(e->getType()));
      } else {
        SILCompoundTypeInfo *cti = baseTy.getCompoundTypeInfo();
        lv.add<FragileElementComponent>(cti->getIndexOfMemberDecl(var),
                                        gen.getLoweredType(e->getType()));
      }
      return ::std::move(lv);
    }
  }
  
  // Otherwise, use the property accessors.
  lv.add<GetterSetterComponent>(SILConstant(decl, SILConstant::Kind::Getter),
                                SILConstant(decl, SILConstant::Kind::Setter),
                                substitutions,
                                e->getType()->getRValueType());
  return ::std::move(lv);
}
  
template<typename ANY_SUBSCRIPT_EXPR>
LValue emitAnySubscriptExpr(SILGenLValue &sgl,
                            SILGenFunction &gen,
                            ANY_SUBSCRIPT_EXPR *e,
                            ArrayRef<Substitution> substitutions) {
  LValue lv = sgl.visitRec(e->getBase());
  SubscriptDecl *sd = e->getDecl();
  lv.add<GetterSetterComponent>(SILConstant(sd, SILConstant::Kind::Getter),
                                SILConstant(sd, SILConstant::Kind::Setter),
                                substitutions,
                                e->getIndex(),
                                e->getType()->getRValueType());
  return ::std::move(lv);
}
  
} // end anonymous namespace

LValue SILGenLValue::visitGenericMemberRefExpr(GenericMemberRefExpr *e) {
  return emitAnyMemberRefExpr(*this, gen, e, e->getSubstitutions());
}

LValue SILGenLValue::visitMemberRefExpr(MemberRefExpr *e) {
  return emitAnyMemberRefExpr(*this, gen, e, {});
}

LValue SILGenLValue::visitGenericSubscriptExpr(GenericSubscriptExpr *e) {
  return emitAnySubscriptExpr(*this, gen, e, e->getSubstitutions());
}

LValue SILGenLValue::visitSubscriptExpr(SubscriptExpr *e) {
  return emitAnySubscriptExpr(*this, gen, e, {});
}

LValue SILGenLValue::visitTupleElementExpr(TupleElementExpr *e) {
  LValue lv = visitRec(e->getBase());
  // FIXME: address-only tuples
  const TypeLoweringInfo &ti = gen.getTypeLoweringInfo(e->getType());
  assert(ti.isLoadable() &&
         "address-only tuples not yet implemented");
  lv.add<FragileElementComponent>(e->getFieldNumber(),
                                  ti.getLoweredType());
  return ::std::move(lv);
}

LValue SILGenLValue::visitAddressOfExpr(AddressOfExpr *e) {
  return visitRec(e->getSubExpr());
}

LValue SILGenLValue::visitParenExpr(ParenExpr *e) {
  return visitRec(e->getSubExpr());
}

LValue SILGenLValue::visitRequalifyExpr(RequalifyExpr *e) {
  assert(e->getType()->is<LValueType>() &&
         "non-lvalue requalify in lvalue expression");
  return visitRec(e->getSubExpr());
}
