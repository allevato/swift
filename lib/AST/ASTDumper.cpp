//===--- ASTDumper.cpp - Swift Language AST Dumper ------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements dumping for the Swift ASTs.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Attr.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/ForeignAsyncConvention.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/PackConformance.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeVisitor.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/QuotedString.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/StringExtras.h"
#include "clang/AST/Type.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>

//
// AST DUMPING TIPS
// ================
//
// * Pass values before names (because names are often optional and can be
//   omitted or empty).
//
// * Put all `printField*()` and `printFlag*()` calls before `printRec()` calls.
//   `printRec()` variants print a child node, and all fields of the current
//   node need to be printed before any child node is printed.
//
// * `printField()` expects a "simple" argument that will be converted to a
//   keyword string by passing it through `getDumpString()`. For values that are
//   at all complicated, use `printFieldQuoted()`, which will automatically
//   quote and escape the value.
//
// * Confine all direct formatting for the console (e.g. printing quotes or
//   parentheses) in `PrintBase`. Eventually we want to allow e.g. JSON dumping;
//   limiting the amount of direct I/O helps us with that.
//

using namespace swift;

struct TerminalColor {
  llvm::raw_ostream::Colors Color;
  bool Bold;
};

#define DEF_COLOR(NAME, COLOR, BOLD) \
static const TerminalColor NAME##Color = { llvm::raw_ostream::COLOR, BOLD };

DEF_COLOR(Func, YELLOW, false)
DEF_COLOR(Range, YELLOW, false)
DEF_COLOR(AccessLevel, YELLOW, false)
DEF_COLOR(ASTNode, YELLOW, true)
DEF_COLOR(Parameter, YELLOW, false)
DEF_COLOR(Extension, MAGENTA, false)
DEF_COLOR(Pattern, RED, true)
DEF_COLOR(Override, RED, false)
DEF_COLOR(Stmt, RED, true)
DEF_COLOR(Captures, RED, false)
DEF_COLOR(Arguments, RED, false)
DEF_COLOR(TypeRepr, GREEN, false)
DEF_COLOR(LiteralValue, GREEN, false)
DEF_COLOR(Decl, GREEN, true)
DEF_COLOR(Parenthesis, BLUE, false)
DEF_COLOR(Type, BLUE, false)
DEF_COLOR(Discriminator, BLUE, false)
DEF_COLOR(InterfaceType, GREEN, false)
DEF_COLOR(Identifier, GREEN, false)
DEF_COLOR(Expr, MAGENTA, true)
DEF_COLOR(ExprModifier, CYAN, false)
DEF_COLOR(DeclModifier, CYAN, false)
DEF_COLOR(ArgModifier, CYAN, false)
DEF_COLOR(ClosureModifier, CYAN, false)
DEF_COLOR(DeclAttribute, CYAN, false)
DEF_COLOR(FieldLabel, CYAN, false)
DEF_COLOR(Location, CYAN, false)

#undef DEF_COLOR

namespace {
  /// RAII object that prints with the given color, if color is supported on the
  /// given stream.
  class PrintWithColorRAII {
    raw_ostream &OS;
    bool ShowColors;

  public:
    PrintWithColorRAII(raw_ostream &os, TerminalColor color)
    : OS(os), ShowColors(false)
    {
      ShowColors = os.has_colors();

      if (ShowColors)
        OS.changeColor(color.Color, color.Bold);
    }

    ~PrintWithColorRAII() {
      if (ShowColors) {
        OS.resetColor();
      }
    }

    raw_ostream &getOS() const { return OS; }

    template<typename T>
    friend raw_ostream &operator<<(PrintWithColorRAII &&printer,
                                   const T &value){
      printer.OS << value;
      return printer.OS;
    }

  };

/// Wraps a \c raw_ostream so that anything printed through it is automatically
/// escaped appropriately for a double-quoted string.
class escaping_ostream : public raw_ostream {
  raw_ostream &base_os;

public:
  escaping_ostream(raw_ostream &base_os)
    : raw_ostream(/*unbuffered=*/true), base_os(base_os)
  {}

  virtual ~escaping_ostream() {}

  virtual void reserveExtraSpace(uint64_t ExtraSize) override {
    base_os.reserveExtraSpace(ExtraSize);
  }

  virtual raw_ostream &changeColor(enum Colors Color, bool Bold = false,
                                   bool BG = false) override {
    return base_os.changeColor(Color, Bold, BG);
  }

  virtual raw_ostream &resetColor() override {
    return base_os.resetColor();
  }

  virtual raw_ostream &reverseColor() override {
    return base_os.reverseColor();
  }

  virtual bool is_displayed() const override {
    return base_os.is_displayed();
  }

  virtual bool has_colors() const override {
    return base_os.has_colors();
  }

  virtual void enable_colors(bool enable) override {
    base_os.enable_colors(enable);
  }

private:
  virtual void write_impl(const char *Ptr, size_t Size) override {
    writeEscaped(StringRef(Ptr, Size), base_os);
  }

  virtual uint64_t current_pos() const override {
    return base_os.tell();
  }

  virtual void anchor() override {}
};
} // end anonymous namespace

static StringRef getDumpString(SILFunctionType::Representation value) {
  switch (value) {
  case SILFunctionType::Representation::Thick: return "thick";
  case SILFunctionType::Representation::Block: return "block";
  case SILFunctionType::Representation::CFunctionPointer: return "c";
  case SILFunctionType::Representation::CXXMethod:
    return "cxx_method";
  case SILFunctionType::Representation::Thin: return "thin";
  case SILFunctionType::Representation::Method: return "method";
  case SILFunctionType::Representation::ObjCMethod: return "objc_method";
  case SILFunctionType::Representation::WitnessMethod: return "witness_method";
  case SILFunctionType::Representation::Closure: return "closure";
  case SILFunctionType::Representation::KeyPathAccessorGetter:
    return "keypath_accessor_getter";
  case SILFunctionType::Representation::KeyPathAccessorSetter:
    return "keypath_accessor_setter";
  case SILFunctionType::Representation::KeyPathAccessorEquals:
    return "keypath_accessor_equals";
  case SILFunctionType::Representation::KeyPathAccessorHash:
    return "keypath_accessor_hash";
  }

  llvm_unreachable("Unhandled SILFunctionTypeRepresentation in switch.");
}

static StringRef getDumpString(ReadImplKind kind) {
  switch (kind) {
  case ReadImplKind::Stored:
    return "stored";
  case ReadImplKind::Inherited:
    return "inherited";
  case ReadImplKind::Get:
    return "getter";
  case ReadImplKind::Address:
    return "addressor";
  case ReadImplKind::Read:
    return "read_coroutine";
  case ReadImplKind::Read2:
    return "read2_coroutine";
  }
  llvm_unreachable("bad kind");
}

static StringRef getDumpString(WriteImplKind kind) {
  switch (kind) {
  case WriteImplKind::Immutable:
    return "immutable";
  case WriteImplKind::Stored:
    return "stored";
  case WriteImplKind::StoredWithObservers:
    return "stored_with_observers";
  case WriteImplKind::InheritedWithObservers:
    return "inherited_with_observers";
  case WriteImplKind::Set:
    return "setter";
  case WriteImplKind::MutableAddress:
    return "mutable_addressor";
  case WriteImplKind::Modify:
    return "modify_coroutine";
  case WriteImplKind::Modify2:
    return "modify2_coroutine";
  }
  llvm_unreachable("bad kind");
}

static StringRef getDumpString(ReadWriteImplKind kind) {
  switch (kind) {
  case ReadWriteImplKind::Immutable:
    return "immutable";
  case ReadWriteImplKind::Stored:
    return "stored";
  case ReadWriteImplKind::MutableAddress:
    return "mutable_addressor";
  case ReadWriteImplKind::MaterializeToTemporary:
    return "materialize_to_temporary";
  case ReadWriteImplKind::Modify:
    return "modify_coroutine";
  case ReadWriteImplKind::Modify2:
    return "modify2_coroutine";
  case ReadWriteImplKind::StoredWithDidSet:
    return "stored_with_didset";
  case ReadWriteImplKind::InheritedWithDidSet:
    return "inherited_with_didset";
  }
  llvm_unreachable("bad kind");
}

static StringRef getDumpString(ImportKind value) {
  switch (value) {
  case ImportKind::Module: return "module";
  case ImportKind::Type: return "type";
  case ImportKind::Struct: return "struct";
  case ImportKind::Class: return "class";
  case ImportKind::Enum: return "enum";
  case ImportKind::Protocol: return "protocol";
  case ImportKind::Var: return "var";
  case ImportKind::Func: return "func";
  }
  
  llvm_unreachable("Unhandled ImportKind in switch.");
}

static StringRef getDumpString(ForeignErrorConvention::Kind value) {
  switch (value) {
  case ForeignErrorConvention::ZeroResult: return "ZeroResult";
  case ForeignErrorConvention::NonZeroResult: return "NonZeroResult";
  case ForeignErrorConvention::ZeroPreservedResult: return "ZeroPreservedResult";
  case ForeignErrorConvention::NilResult: return "NilResult";
  case ForeignErrorConvention::NonNilError: return "NonNilError";
  }

  llvm_unreachable("Unhandled ForeignErrorConvention in switch.");
}
static StringRef getDumpString(DefaultArgumentKind value) {
  switch (value) {
    case DefaultArgumentKind::None: return "none";
#define MAGIC_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
    case DefaultArgumentKind::NAME: return STRING;
#include "swift/AST/MagicIdentifierKinds.def"
    case DefaultArgumentKind::Inherited: return "inherited";
    case DefaultArgumentKind::NilLiteral: return "nil";
    case DefaultArgumentKind::EmptyArray: return "[]";
    case DefaultArgumentKind::EmptyDictionary: return "[:]";
    case DefaultArgumentKind::Normal: return "normal";
    case DefaultArgumentKind::StoredProperty: return "stored property";
    case DefaultArgumentKind::ExpressionMacro:
    return "expression macro";
  }

  llvm_unreachable("Unhandled DefaultArgumentKind in switch.");
}
static StringRef getDumpString(ObjCSelectorExpr::ObjCSelectorKind value) {
  switch (value) {
    case ObjCSelectorExpr::Method: return "method";
    case ObjCSelectorExpr::Getter: return "getter";
    case ObjCSelectorExpr::Setter: return "setter";
  }

  llvm_unreachable("Unhandled ObjCSelectorExpr in switch.");
}
static StringRef getDumpString(AccessSemantics value) {
  switch (value) {
    case AccessSemantics::Ordinary: return "ordinary";
    case AccessSemantics::DirectToStorage: return "direct_to_storage";
    case AccessSemantics::DirectToImplementation: return "direct_to_impl";
    case AccessSemantics::DistributedThunk: return "distributed_thunk";
  }

  llvm_unreachable("Unhandled AccessSemantics in switch.");
}
static StringRef getDumpString(MetatypeRepresentation value) {
  switch (value) {
    case MetatypeRepresentation::Thin: return "thin";
    case MetatypeRepresentation::Thick: return "thick";
    case MetatypeRepresentation::ObjC: return "@objc";
  }

  llvm_unreachable("Unhandled MetatypeRepresentation in switch.");
}
static StringRef getDumpString(StringLiteralExpr::Encoding value) {
  switch (value) {
    case StringLiteralExpr::UTF8: return "utf8";
    case StringLiteralExpr::OneUnicodeScalar: return "unicodeScalar";
  }

  llvm_unreachable("Unhandled StringLiteral in switch.");
}
static StringRef getDumpString(CtorInitializerKind value) {
  switch (value) {
    case CtorInitializerKind::Designated: return "designated";
    case CtorInitializerKind::Convenience: return "convenience";
    case CtorInitializerKind::ConvenienceFactory: return "convenience_factory";
    case CtorInitializerKind::Factory: return "factory";
  }

  llvm_unreachable("Unhandled CtorInitializerKind in switch.");
}
static StringRef getDumpString(Associativity value) {
  switch (value) {
    case Associativity::None: return "none";
    case Associativity::Left: return "left";
    case Associativity::Right: return "right";
  }

  llvm_unreachable("Unhandled Associativity in switch.");
}
static StringRef getDumpString(CheckedCastKind kind) {
  return getCheckedCastKindName(kind);
}
static StringRef getDumpString(bool value) {
  return value ? "true" : "false";
}
static StringRef getDumpString(AccessLevel level) {
  return getAccessLevelSpelling(level);
}
static StringRef getDumpString(LifetimeAnnotation lifetime) {
  switch (lifetime) {
  case LifetimeAnnotation::EagerMove:
    return "_eagerMove";
  case LifetimeAnnotation::Lexical:
    return "_lexical";
  case LifetimeAnnotation::None:
    return "";
  }
  
  llvm_unreachable("Unhandled LifetimeAnnotation in switch.");
}
static StringRef getDumpString(AccessorKind kind) {
  return getAccessorKindString(kind);
}
static StringRef getDumpString(MagicIdentifierLiteralExpr::Kind kind) {
  return MagicIdentifierLiteralExpr::getKindString(kind);
}
static StringRef getDumpString(ObjectLiteralExpr::LiteralKind kind) {
  return ObjectLiteralExpr::getLiteralKindPlainName(kind);
}
static StringRef getDumpString(ParamSpecifier specifier) {
  return ParamDecl::getSpecifierSpelling(specifier);
}
static StringRef getDumpString(ValueOwnership ownership) {
  switch (ownership) {
  case ValueOwnership::Default:
      return "";
  case ValueOwnership::Owned:
      return "owned";
  case ValueOwnership::Shared:
      return "shared";
  case ValueOwnership::InOut:
      return "inout";
  }

  llvm_unreachable("Unhandled ValueOwnership in switch.");
}
static StringRef getDumpString(PlatformKind kind) {
  return platformString(kind);
}
static StringRef getDumpString(ForeignErrorConvention::IsOwned_t owned) {
  switch (owned) {
  case swift::ForeignErrorConvention::IsNotOwned:
    return "unowned";
  case swift::ForeignErrorConvention::IsOwned:
    return "owned";
  }

  llvm_unreachable("Unhandled ForeignErrorConvention::IsOwned_t in switch.");
}
static StringRef getDumpString(RequirementKind kind) {
  switch (kind) {
    case RequirementKind::SameShape: return "same_shape";
    case RequirementKind::Conformance: return "conforms_to";
    case RequirementKind::Layout: return "has_layout";
    case RequirementKind::Superclass: return "subclass_of";
    case RequirementKind::SameType: return "same_type";
  }

  llvm_unreachable("Unhandled RequirementKind in switch.");
}
static StringRef getDumpString(ClangImporterSynthesizedTypeAttr::Kind kind) {
  switch (kind) {
  case ClangImporterSynthesizedTypeAttr::Kind::NSErrorWrapper:
    return "NSErrorWrapper";
  case ClangImporterSynthesizedTypeAttr::Kind::NSErrorWrapperAnon:
    return "NSErrorWrapperAnon";
  }
  llvm_unreachable("unhandled ClangImporterSynthesizedTypeAttr::Kind");
}
static StringRef getDumpString(ExternKind kind) {
  switch (kind) {
  case ExternKind::C:
    return "C";
  case ExternKind::Wasm:
    return "Wasm";
  }
  llvm_unreachable("unhandled ExternKind");
}
static StringRef getDumpString(InlineKind kind) {
  switch (kind) {
  case InlineKind::Always:
    return "always";
  case InlineKind::Never:
    return "never";
  }
  llvm_unreachable("unhandled InlineKind");
}
static StringRef getDumpString(MacroRole role) {
  return getMacroRoleString(role);
}
static StringRef getDumpString(EffectsKind kind) {
  switch (kind) {
  case EffectsKind::ReadNone:
    return "ReadNone";
  case EffectsKind::ReadOnly:
    return "ReadOnly";
  case EffectsKind::ReleaseNone:
    return "ReleaseNone";
  case EffectsKind::ReadWrite:
    return "ReadWrite";
  case EffectsKind::Unspecified:
    return "Unspecified";
  case EffectsKind::Custom:
    return "Custom";
  }
  llvm_unreachable("unhandled EffectsKind");
}
static StringRef getDumpString(ExclusivityAttr::Mode mode) {
  switch (mode) {
  case ExclusivityAttr::Mode::Checked:
    return "checked";
  case ExclusivityAttr::Mode::Unchecked:
    return "unchecked";
  }
  llvm_unreachable("unhandled ExclusivityAttr::Mode");
}
static StringRef getDumpString(OptimizationMode mode) {
  switch (mode) {
  case OptimizationMode::NotSet:
    return "<not set>";
  case OptimizationMode::NoOptimization:
    return "NoOptimization";
  case OptimizationMode::ForSpeed:
    return "ForSpeed";
  case OptimizationMode::ForSize:
    return "ForSize";
  }
}
static StringRef getDumpString(NonSendableKind kind) {
  switch (kind) {
  case NonSendableKind::Assumed:
    return "Assumed";
  case NonSendableKind::Specific:
    return "Specific";
  }
}
static StringRef getDumpString(StringRef s) {
  return s;
}
static unsigned getDumpString(unsigned value) {
  return value;
}
static size_t getDumpString(size_t value) {
  return value;
}
static void *getDumpString(void *value) { return value; }

static StringRef getDumpString(Identifier ident) { return ident.str(); }

//===----------------------------------------------------------------------===//
//  Decl printing.
//===----------------------------------------------------------------------===//

// Print a name.
static void printName(raw_ostream &os, DeclName name) {
  if (!name)
    os << "<anonymous>";
  else
    os << name;
}

static Type defaultGetTypeOfExpr(Expr *E) { return E->getType(); }
static Type defaultGetTypeOfKeyPathComponent(KeyPathExpr *E, unsigned index) {
  return E->getComponents()[index].getComponentType();
}

using VisitedConformances = llvm::SmallPtrSetImpl<const ProtocolConformance *>;

namespace {
  /// Encapsulates a label attached to some data in the AST being dumped.
  ///
  /// This type mainly exists to balance the simplified S-expression output
  /// with additional structure needed for more complex data formats like
  /// JSON. The S-expression output, in most cases, prints nested data as
  /// unlabeled children (indented one more step) and allows fields to be
  /// printed without explicit names, but this isn't compatible with JSON where
  /// each value needs its own unique label/key (or it needs to explicitly be an
  /// element of an array). When this label is constructed from a `StringRef`,
  /// it is treated as an "unconditional" label that both the S-expression
  /// format and the JSON format will print. If this is constructed using the
  /// `suppressable` factory method, it will only be used by the JSON writer.
  struct PrintLabel {
    StringRef Text;
    bool Suppressable;

    PrintLabel(const char *text) : Text(text), Suppressable(false) {}
    PrintLabel(StringRef text) : Text(text), Suppressable(false) {}
    operator StringRef() const { return Text; }

    static PrintLabel suppressable(StringRef text) {
      PrintLabel label(text);
      label.Suppressable = true;
      return label;
    }
  };

  /// Defines the interface for low-level printing operations that take place
  /// when dumping a Swift AST.
  class PrintWriterBase {
  protected:
    /// Only used by the S-expression writer to change the dumper to use
    /// single quotes when printing substitution maps in full.
    char Quote = '\"';

  public:
    virtual ~PrintWriterBase() {}

    char quote() const { return Quote; }
    void setQuote(char quote) { Quote = quote; }

    /// Call `Body` in a context where the printer is ready for a child to be
    /// printed.
    virtual void printRecArbitrary(std::function<void(PrintLabel)> Body,
                                   PrintLabel Label) = 0;

    /// Print a range of nodes as a single "array" child node.
    virtual void printRecRange(std::function<void(PrintLabel)> Body,
                               PrintLabel Label) = 0;

    /// Call `Body` in a context where the printer is ready for a list of
    /// children to be printed.
    virtual void printListArbitrary(PrintLabel Label,
                                    std::function<void()> Body) = 0;

    /// Print the beginning of a new node, including its type and an optional
    /// label for it.
    virtual void printHead(StringRef Name, TerminalColor Color,
                           PrintLabel Label) = 0;

    /// Print the end of a new node.
    virtual void printFoot() = 0;

    /// Print a field with a short keyword-style value, printing the value by
    /// passing a closure that takes a \c raw_ostream.
    virtual void printFieldRaw(std::function<void(llvm::raw_ostream &)> Body,
                               PrintLabel Label, TerminalColor Color) = 0;

    /// Print a field with a long value that will be automatically quoted and
    /// escaped, printing the value by passing a closure that takes a
    /// \c raw_ostream.
    virtual void printFieldQuotedRaw(
        std::function<void(llvm::raw_ostream &)> Body, PrintLabel Name,
        TerminalColor Color) = 0;

    /// Print a simple boolean value, printing the value by passing a closure
    /// that takes a \c raw_ostream.
    virtual void printFlagRaw(std::function<void(llvm::raw_ostream &)> Body,
                              TerminalColor Color) = 0;

    /// Print a field containing a node's source location.
    virtual void printSourceLoc(const SourceLoc L, const ASTContext *Ctx,
                                PrintLabel Label = "location") = 0;

    /// Print a field containing a node's source range.
    virtual void printSourceRange(const SourceRange R,
                                  const ASTContext *Ctx) = 0;

    virtual bool hasNonStandardOutput() const = 0;

    /// Indicates whether the output format is meant to be parsable. Parsable
    /// output should use structure rather than stringification to convey
    /// detailed information, and generally provides more information than the
    /// non-parsable formats, which are usually meant for human debugging.
    virtual bool isParsable() const = 0;
  };

  /// Implements the default (pseudo-S-expression) output format for `-dump-ast`.
  class DefaultWriter : public PrintWriterBase {
    raw_ostream &OS;
    unsigned Indent;
  public:
    DefaultWriter(raw_ostream &os, unsigned indent) : OS(os), Indent(indent) {}

    void printRecArbitrary(std::function<void(PrintLabel)> Body,
                           PrintLabel Label) override {
      Indent += 2;
      OS << '\n';
      Body(Label);
      Indent -= 2;
    }

    void printRecRange(std::function<void(PrintLabel)> Body,
                       PrintLabel Label) override {
      printRecArbitrary([&](PrintLabel Label) {
        printHead("array", ASTNodeColor, Label);
        Body(Label);
        printFoot();
      }, Label);
    }

    void printListArbitrary(PrintLabel Label,
                            std::function<void()> Body) override {
      // This writer ignores the label and simply prints the list directly
      // underneath its parent.
      Body();
    }

    void printHead(StringRef Name, TerminalColor Color,
                   PrintLabel Label) override {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      if (!Label.Text.empty() && !Label.Suppressable) {
        PrintWithColorRAII(OS, FieldLabelColor) << Label;
        OS << "=";
      }
      PrintWithColorRAII(OS, Color) << Name;
    }

    void printFoot() override {
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printFieldRaw(std::function<void(llvm::raw_ostream &)> Body,
                       PrintLabel Label, TerminalColor Color) override {
      OS << " ";
      if (!Label.Text.empty() && !Label.Suppressable)
        PrintWithColorRAII(OS, Color) << Label << "=";
      std::string value;
      llvm::raw_string_ostream SOS(value);
      Body(SOS);
      PrintWithColorRAII(OS, Color) << value;
    }

    void printFieldQuotedRaw(std::function<void(llvm::raw_ostream &)> Body,
                             PrintLabel Name, TerminalColor Color) override {
      printFieldRaw([&](raw_ostream &OS) {
        OS << Quote;
        { escaping_ostream escOS(OS); Body(escOS); }
        OS << Quote;
      }, Name, Color);
    }

    void printFlagRaw(std::function<void(llvm::raw_ostream &)> Body,
                      TerminalColor Color) override {
      printFieldRaw(Body, "", Color);
    }

    void printSourceLoc(const SourceLoc L, const ASTContext *Ctx,
                       PrintLabel Label) override {
      printFieldRaw([&](raw_ostream &OS) {
        escaping_ostream escOS(OS);
        L.print(escOS, Ctx->SourceMgr);
      }, Label.Text, LocationColor);
    }

    void printSourceRange(const SourceRange R,
                          const ASTContext *Ctx) override {
      printFieldRaw([&](raw_ostream &OS) {
        escaping_ostream escOS(OS);
        R.print(escOS, Ctx->SourceMgr, /*PrintText=*/false);
      }, "range", RangeColor);
    }

    bool hasNonStandardOutput() const override {
      return &OS != &llvm::errs() && &OS != &llvm::dbgs();
    }

    bool isParsable() const override { return false; }
  };

  /// Implements JSON formatted output for `-ast-dump`.
  class JSONWriter : public PrintWriterBase {
    llvm::json::OStream OS;
    std::vector<bool> InObjectStack;

  public:
    JSONWriter(raw_ostream &os, unsigned indent = 0) : OS(os, indent) {}

    void printRecArbitrary(std::function<void(PrintLabel)> Body,
                           PrintLabel Label) override {
      if (InObjectStack.back()) {
        OS.attributeBegin(Label);
        Body("");
        OS.attributeEnd();
      } else {
        Body(Label);
      }
    }

    void printRecRange(std::function<void(PrintLabel)> Body,
                       PrintLabel Label) override {
      printListArbitrary(Label, [&]{ Body(Label); });
    }

    void printListArbitrary(PrintLabel Label,
                            std::function<void()> Body) override {
      OS.attributeBegin(Label);
      OS.arrayBegin();
      InObjectStack.push_back(false);
      Body();
      InObjectStack.pop_back();
      OS.arrayEnd();
      OS.attributeEnd();
    }

    void printHead(StringRef Name, TerminalColor Color,
                   PrintLabel Label) override {
      OS.objectBegin();
      InObjectStack.push_back(true);
      OS.attribute(Label.Text.empty() ? "_kind" : Label, Name);
    }

    void printFoot() override {
      InObjectStack.pop_back();
      OS.objectEnd();
    }

    void printFieldRaw(std::function<void(llvm::raw_ostream &)> Body,
                       PrintLabel Label, TerminalColor Color) override {
      std::string value;
      llvm::raw_string_ostream SOS(value);
      Body(SOS);
      OS.attribute(Label.Text, value);
    }

    void printFieldQuotedRaw(std::function<void(llvm::raw_ostream &)> Body,
                             PrintLabel Label, TerminalColor Color) override {
      // No need to do special quoting for complex values; the JSON output
      // stream will do this for us.
      printFieldRaw(Body, Label, Color);
    }

    void printFlagRaw(std::function<void(llvm::raw_ostream &)> Body,
                      TerminalColor Color) override {
      std::string flag;
      llvm::raw_string_ostream SOS(flag);
      Body(SOS);
      OS.attribute(flag, true);
    }

    void printSourceLoc(const SourceLoc L, const ASTContext *Ctx,
                        PrintLabel label) override {
      // For compactness, we only print source ranges in JSON, since they
      // provide a superset of this information.
    }

    void printSourceRange(const SourceRange R,
                          const ASTContext *Ctx) override {
      OS.attributeBegin("range");
      OS.objectBegin();

      SourceManager &srcMgr = Ctx->SourceMgr;
      unsigned startBufferID = srcMgr.findBufferContainingLoc(R.Start);
      unsigned startOffset = srcMgr.getLocOffsetInBuffer(R.Start,
                                                         startBufferID);
      OS.attribute("start", startOffset);

      unsigned endBufferID = srcMgr.findBufferContainingLoc(R.End);
      unsigned endOffset = srcMgr.getLocOffsetInBuffer(R.End, endBufferID);
      OS.attribute("end", endOffset);

      OS.objectEnd();
      OS.attributeEnd();
    }

    bool hasNonStandardOutput() const override { return true; }

    bool isParsable() const override { return true; }
  };

  /// PrintBase - Base type for recursive structured dumps of AST nodes.
  ///
  /// Please keep direct I/O, especially of structural elements like
  /// parentheses and quote marks, confined to this base class. This will help
  /// if we eventually support alternate output formats for AST dumps.
  class PrintBase {
  protected:
    PrintWriterBase &Writer;
  public:
    bool ParseIfNeeded;
    llvm::function_ref<Type(Expr *)> GetTypeOfExpr;
    llvm::function_ref<Type(TypeRepr *)> GetTypeOfTypeRepr;
    llvm::function_ref<Type(KeyPathExpr *E, unsigned index)>
        GetTypeOfKeyPathComponent;
    char quote = '"';

    PrintBase(
        PrintWriterBase &writer, bool parseIfNeeded = false,
        llvm::function_ref<Type(Expr *)> getTypeOfExpr = defaultGetTypeOfExpr,
        llvm::function_ref<Type(TypeRepr *)> getTypeOfTypeRepr = nullptr,
        llvm::function_ref<Type(KeyPathExpr *E, unsigned index)>
            getTypeOfKeyPathComponent = defaultGetTypeOfKeyPathComponent)
        : Writer(writer), ParseIfNeeded(parseIfNeeded),
          GetTypeOfExpr(getTypeOfExpr), GetTypeOfTypeRepr(getTypeOfTypeRepr),
          GetTypeOfKeyPathComponent(getTypeOfKeyPathComponent) {}

    bool hasNonStandardOutput() {
      return Writer.hasNonStandardOutput();
    }

    /// Call `Body` in a context where the printer is ready for a child to be
    /// printed.
    void printRecArbitrary(std::function<void(PrintLabel)> Body,
                           PrintLabel label = "") {
      Writer.printRecArbitrary(Body, label);
    }

    /// Print a declaration as a child node.
    void printRec(Decl *D, PrintLabel label = "");

    /// Print an expression as a child node.
    void printRec(Expr *E, PrintLabel label = "");

    /// Print a statement as a child node.
    void printRec(Stmt *S, const ASTContext *Ctx, PrintLabel label = "");

    /// Print a type representation as a child node.
    void printRec(TypeRepr *T, PrintLabel label = "");

    /// Print a pattern as a child node.
    void printRec(const Pattern *P, PrintLabel label = "");

    /// Print a type as a child node.
    void printRec(Type ty, PrintLabel label = "");

    /// Print an attribute as a child node.
    void printRec(const DeclAttribute *Attr, const ASTContext *Ctx,
                  PrintLabel label = "");

    /// Print an \c ASTNode as a child node.
    void printRec(const ASTNode &Elt, const ASTContext *Ctx,
                  PrintLabel label = "") {
      if (auto *SubExpr = Elt.dyn_cast<Expr*>())
        printRec(SubExpr, label);
      else if (auto *SubStmt = Elt.dyn_cast<Stmt*>())
        printRec(SubStmt, Ctx, label);
      else
        printRec(Elt.get<Decl*>(), label);
    }

    /// Print a statement condition element as a child node.
    void printRec(StmtConditionElement C, const ASTContext *Ctx,
                  PrintLabel Label = "") {
      switch (C.getKind()) {
      case StmtConditionElement::CK_Boolean:
        return printRec(C.getBoolean());
      case StmtConditionElement::CK_PatternBinding:
          printRecArbitrary([&](PrintLabel Label) {
            printHead("pattern", PatternColor, Label);
            printRec(C.getPattern(), PrintLabel::suppressable("pattern"));
            printRec(C.getInitializer(),
                     PrintLabel::suppressable("initializer"));
            printFoot();
          }, Label);
        break;
      case StmtConditionElement::CK_Availability:
        printRecArbitrary([&](PrintLabel Label) {
          printHead("#available", PatternColor, Label);
          printList("queries", C.getAvailability()->getQueries(),
                    [&](auto *Query) { printRec(Query); });
          printFoot();
        }, Label);
        break;
      case StmtConditionElement::CK_HasSymbol:
        printRecArbitrary([&](PrintLabel Label) {
          printHead("#_hasSymbol", PatternColor, Label);
          printSourceRange(C.getSourceRange(), Ctx);
          printRec(C.getHasSymbolInfo()->getSymbolExpr(),
                   PrintLabel::suppressable("symbol_expr"));
          printFoot();
        }, Label);
        break;
      }
    }

    /// Print an availability spec as a child node.
    void printRec(AvailabilitySpec *Spec) {
      printRecArbitrary(
          [&](PrintLabel Label) {
            switch (Spec->getKind()) {
            case AvailabilitySpecKind::PlatformVersionConstraint: {
              auto plat = cast<PlatformVersionConstraintAvailabilitySpec>(Spec);
              printHead("platform_version_constraint_availability_spec",
                        PatternColor, Label);
              printField(platformString(plat->getPlatform()), "platform");
              printFieldRaw(
                  [&](llvm::raw_ostream &OS) { OS << plat->getVersion(); },
                  "version");
              printFoot();
              break;
            }
            case AvailabilitySpecKind::LanguageVersionConstraint:
            case AvailabilitySpecKind::PackageDescriptionVersionConstraint: {
              auto agnostic =
                  cast<PlatformAgnosticVersionConstraintAvailabilitySpec>(Spec);
              printHead("platform_agnostic_version_constraint_"
                        "availability_spec",
                        PatternColor, Label);
              printField(agnostic->isLanguageVersionSpecific()
                             ? "swift"
                             : "package_description",
                         "kind");
              printFieldRaw(
                  [&](llvm::raw_ostream &OS) { OS << agnostic->getVersion(); },
                  "version");
              printFoot();
              break;
            }
            case AvailabilitySpecKind::OtherPlatform:
              printHead("other_constraint_availability_spec", PatternColor,
                        Label);
              printFoot();
              break;
            }
          });
    }

    /// Print a range of nodes as a single "array" child node.
    template <typename NodeRange>
    void printRecRange(const NodeRange &range, PrintLabel topLabel) {
      Writer.printRecRange([&](PrintLabel topLabel) {
        for (auto node : range) {
          printRec(node, "");
        }
      }, topLabel);
    }

    /// Print a range of nodes as a single "array" child node.
    template <typename NodeRange>
    void printRecRange(const NodeRange &range, const ASTContext *Ctx,
                       PrintLabel topLabel) {
      Writer.printRecRange([&](PrintLabel topLabel) {
        for (auto node : range) {
          printRec(node, Ctx, "");
        }
      }, topLabel);
    }

    /// Print the beginning of a new node, including its type and an optional
    /// label for it.
    void printHead(StringRef Name, TerminalColor Color,
                   PrintLabel Label = "") {
      Writer.printHead(Name, Color, Label);
    }

    /// Print the end of a new node.
    void printFoot() {
      Writer.printFoot();
    }

    /// Prints a list of items, calling the given function to process each
    /// element. Nothing is printed (not even an empty list) if the list is
    /// empty.
    template <typename T, typename F>
    void printList(PrintLabel Label, const T& List, F Fn) {
      // Cannot use `empty()` here because it's not implemented by some
      // otherwise iterable types.
      if (List.begin() == List.end())
        return;

      Writer.printListArbitrary(Label, [&]{
        for (const auto &Elem : List) {
          Fn(Elem);
        }
      });
    }

    /// Prints any structure necessary to render a list of items, calling
    /// the given function to produce the contents of the list.
    void printListArbitrary(PrintLabel Label, std::function<void()> Body) {
      Writer.printListArbitrary(Label, Body);
    }

    /// Print a single argument as a child node.
    void printRec(const Argument &arg) {
      printRecArbitrary([&](PrintLabel L) {
        printHead("argument", ExprColor, L);

        auto label = arg.getLabel();
        if (!label.empty()) {
          printFieldQuoted(label.str(), "label", ArgumentsColor);
        }
        printFlag(arg.isInOut(), "inout", ArgModifierColor);

        printRec(arg.getExpr(), PrintLabel::suppressable("expr"));
        printFoot();
      });
    }

    /// Print an argument list as a child node.
    void printRec(const ArgumentList *argList, PrintLabel label = "") {
      printRecArbitrary([&](PrintLabel label) {
        visitArgumentList(argList, label);
      }, label);
    }

    /// Print an argument list node.
    void visitArgumentList(const ArgumentList *argList,
                           PrintLabel label = "") {
      printHead("argument_list", ExprColor, label);

      printFlag(argList->isImplicit(), "implicit", ArgModifierColor);

      if (argList->hasAnyArgumentLabels()) {
        printFieldQuotedRaw([&](raw_ostream &OS) {
          for (auto arg : *argList) {
            auto label = arg.getLabel();
            OS << (label.empty() ? "_" : label.str()) << ":";
          }
        }, "labels", ArgumentsColor);
      }

      printList("args", *argList, [&](auto arg) {
        printRec(arg);
      });

      printFoot();
    }

    /// Print a parameter list as a child node.
    void printRec(const ParameterList *params, const ASTContext *ctx = nullptr,
                  PrintLabel label = "") {
      printRecArbitrary([&](PrintLabel label) {
        visitParameterList(params, ctx, label);
      }, label);
    }

    /// Print a parameter list node.
    void visitParameterList(const ParameterList *params,
                            const ASTContext *ctx = nullptr,
                            PrintLabel label = "") {
      printHead("parameter_list", ParameterColor, label);

      if (!ctx && params->size() != 0 && params->get(0))
        ctx = &params->get(0)->getASTContext();
      printSourceRange(params->getSourceRange(), ctx);

      printList("parameters", *params, [&](auto P) {
        printRec(const_cast<ParamDecl *>(P));
      });

      printFoot();
    }

    /// Print an \c IfConfigClause as a child node.
    void printRec(const IfConfigClause &Clause, const ASTContext *Ctx = nullptr,
                  PrintLabel Label = "") {
      printRecArbitrary([&](PrintLabel Label) {
        printHead((Clause.Cond ? "#if:" : "#else:"), StmtColor, Label);

        printFlag(Clause.isActive, "active", DeclModifierColor);

        if (Clause.Cond) {
          printRec(Clause.Cond);
        }
        printRecRange(Clause.Elements, Ctx, "elements");

        printFoot();
      }, Label);
    }

    /// Print a substitution map as a child node.
    void printRec(SubstitutionMap map, PrintLabel label = "") {
      SmallPtrSet<const ProtocolConformance *, 4> Dumped;
      printRec(map, Dumped, label);
    }

    /// Print a substitution map as a child node.
    void printRec(SubstitutionMap map, VisitedConformances &visited,
                  PrintLabel label = "");

    /// Print a substitution map as a child node.
    void printRec(const ProtocolConformanceRef &conf,
                  VisitedConformances &visited, PrintLabel label = "");

    /// Print a conformance reference as a child node.
    void printRec(const ProtocolConformanceRef &conf, PrintLabel label = "") {
      SmallPtrSet<const ProtocolConformance *, 4> Dumped;
      printRec(conf, Dumped, label);
    }

    /// Print a conformance reference as a child node.
    void printRec(const ProtocolConformance *conformance,
                  VisitedConformances &visited, PrintLabel label = "");

    /// Print a requirement node.
    void visitRequirement(const Requirement &requirement,
                          PrintLabel label = "") {
      printHead("requirement", ASTNodeColor, label);

      PrintOptions opts;
      opts.ProtocolQualifiedDependentMemberTypes = true;

      printFieldQuotedRaw([&](raw_ostream &out) {
        requirement.getFirstType().print(out, opts);
      }, "");

      printField(requirement.getKind(), "");

      if (requirement.getKind() != RequirementKind::Layout
            && requirement.getSecondType())
        printFieldQuotedRaw([&](raw_ostream &out) {
          requirement.getSecondType().print(out, opts);
        }, "");
      else if (requirement.getLayoutConstraint())
        printFieldQuoted(requirement.getLayoutConstraint(), "");

      printFoot();
    }

    /// Print a requirement as a child node.
    void printRec(const Requirement &requirement, PrintLabel label = "") {
      printRecArbitrary([&](PrintLabel label) {
        visitRequirement(requirement);
      });
    }

    /// Print a field with a short keyword-style value, printing the value by
    /// passing a closure that takes a \c raw_ostream.
    template<typename Fn>
    void printFieldRaw(Fn body, PrintLabel name,
                       TerminalColor color = FieldLabelColor) {
      Writer.printFieldRaw([&](raw_ostream &OS) { body(OS); }, name, color);
    }

    /// Print a field with a short keyword-style value. The value will be
    /// formatted using a \c getDumpString() overload.
    template<typename T>
    void printField(const T &value, PrintLabel name,
                    TerminalColor color = FieldLabelColor) {
      printFieldRaw([&](raw_ostream &OS) { OS << getDumpString(value); },
                    name, color);
    }

    /// Print a field with a long value that will be automatically quoted and
    /// escaped, printing the value by passing a closure that takes a
    /// \c raw_ostream.
    template<typename Fn>
    void printFieldQuotedRaw(Fn body, PrintLabel name,
                             TerminalColor color = FieldLabelColor) {
      Writer.printFieldQuotedRaw(body, name, color);
    }

    /// Print a field with a long value that will be automatically quoted and
    /// escaped.
    template<typename T>
    void printFieldQuoted(const T &value, PrintLabel name,
                          TerminalColor color = FieldLabelColor) {
      printFieldQuotedRaw([&](raw_ostream &OS) { OS << value; }, name, color);
    }

    /// Print a simple boolean value, printing the value by passing a closure
    /// that takes a \c raw_ostream.
    template<typename Fn>
    void printFlagRaw(Fn body, TerminalColor color = FieldLabelColor) {
      Writer.printFlagRaw(body, color);
    }

    /// Print a simple boolean value unconditionally.
    void printFlag(StringRef name, TerminalColor color = FieldLabelColor) {
      printFlagRaw([&](raw_ostream &OS) { OS << name; }, color);
    }

    /// Print a simple boolean value.
    void printFlag(bool isSet, StringRef name,
                   TerminalColor color = FieldLabelColor) {
      if (isSet)
        printFlag(name, color);
    }

    /// Print a field containing a node's source location.
    void printSourceLoc(const SourceLoc L, const ASTContext *Ctx,
                       PrintLabel label = "location") {
      if (!L.isValid() || !Ctx)
        return;
      Writer.printSourceLoc(L, Ctx, label);
    }

    /// Print a field containing a node's source range.
    void printSourceRange(const SourceRange R, const ASTContext *Ctx) {
      if (!R.isValid() || !Ctx)
        return;
      Writer.printSourceRange(R, Ctx);
    }

    /// Print a field containing a node's name, printing the node's name by
    /// passing a closure that takes a \c raw_ostream.
    template <typename Fn>
    void printNameRaw(Fn body, PrintLabel Label = "") {
      if (Label.Text.empty()) {
        // If we were given an empty name, make sure we have a suitable default
        // fallback for parsable output formats.
        Label = PrintLabel::suppressable("name");
      }
      printFieldQuotedRaw([&](raw_ostream &OS) { body(OS); }, Label,
                          IdentifierColor);
    }

    /// Print a field containing a node's name.
    void printName(DeclName name, PrintLabel Label = "") {
      printNameRaw([&](raw_ostream &OS) {
        ::printName(OS, name);
      }, Label);
    }

    /// Print an unnamed field containing a node's name, read from a declaration.
    void printDeclName(const ValueDecl *D, PrintLabel Label = "") {
      if (D->getName()) {
        printName(D->getName(), Label);
      } else {
        if (Label.Text.empty()) {
          // If we were given an empty name, make sure we have a suitable default
          // fallback for parsable output formats.
          Label = PrintLabel::suppressable("name");
        }
        printFieldRaw([&](raw_ostream &OS) {
          OS << "<anonymous @ " << (const void*)D << '>';
        }, Label, IdentifierColor);
      }
    }

    /// Print a field containing a concrete reference to a declaration.
    void printDeclRefField(ConcreteDeclRef declRef, PrintLabel label,
                           TerminalColor Color = DeclColor) {
      printFieldQuotedRaw([&](raw_ostream &OS) { declRef.dump(OS); }, label,
                          Color);
    }

    void printThrowDest(ThrownErrorDestination throws, bool wantNothrow) {
      if (!throws) {
        if (wantNothrow)
          printFlag("nothrow", ExprModifierColor);

        return;
      }

      auto thrownError = throws.getThrownErrorType();
      auto contextError = throws.getContextErrorType();
      if (thrownError->isEqual(contextError)) {
        // No translation of the thrown error type is required, so ony print
        // the thrown error type.
        Type errorExistentialType =
            contextError->getASTContext().getErrorExistentialType();
        if (errorExistentialType && thrownError->isEqual(errorExistentialType))
          printFlag("throws", ExprModifierColor);
        else {
          printFlag("throws(" + thrownError.getString() + ")", ExprModifierColor);
        }
        return;
      }

      printFlag("throws(" + thrownError.getString() + " to " +
                contextError.getString() + ")", ExprModifierColor);
    }
  };

  class PrintPattern : public PatternVisitor<PrintPattern, void, PrintLabel>,
                       public PrintBase {
  public:
    using PrintBase::PrintBase;

    void printCommon(Pattern *P, const char *Name, PrintLabel Label) {
      printHead(Name, PatternColor, Label);

      printFlag(P->isImplicit(), "implicit", ExprModifierColor);

      if (P->hasType()) {
        printFieldQuoted(P->getType(), "type", TypeColor);
      }
    }

    void visitParenPattern(ParenPattern *P, PrintLabel label) {
      printCommon(P, "pattern_paren", label);
      printRec(P->getSubPattern(), PrintLabel::suppressable("sub_pattern"));
      printFoot();
    }
    void visitTuplePattern(TuplePattern *P, PrintLabel label) {
      printCommon(P, "pattern_tuple", label);

      printFieldQuotedRaw([&](raw_ostream &OS) {
        interleave(P->getElements(), OS,
                   [&](const TuplePatternElt &elt) {
                     auto name = elt.getLabel();
                     OS << (name.empty() ? "''" : name.str());
                   }, ",");
      }, "names");

      printList("elements", P->getElements(), [&](auto &elt) {
        printRec(elt.getPattern());
      });
      printFoot();
    }
    void visitNamedPattern(NamedPattern *P, PrintLabel label) {
      printCommon(P, "pattern_named", label);
      printDeclName(P->getDecl());
      printFoot();
    }
    void visitAnyPattern(AnyPattern *P, PrintLabel label) {
      if (P->isAsyncLet()) {
        printCommon(P, "async_let ", label);
      }
      printCommon(P, "pattern_any", label);
      printFoot();
    }
    void visitTypedPattern(TypedPattern *P, PrintLabel label) {
      printCommon(P, "pattern_typed", label);
      printRec(P->getSubPattern(), PrintLabel::suppressable("sub_pattern"));
      if (auto *repr = P->getTypeRepr()) {
        printRec(repr, PrintLabel::suppressable("type_repr"));
      }
      printFoot();
    }

    void visitIsPattern(IsPattern *P, PrintLabel label) {
      printCommon(P, "pattern_is", label);
      printField(P->getCastKind(), "cast_kind");
      printFieldQuoted(P->getCastType(), "cast_to", TypeColor);
      if (auto sub = P->getSubPattern()) {
        printRec(sub, PrintLabel::suppressable("sub_pattern"));
      }
      printFoot();
    }
    void visitExprPattern(ExprPattern *P, PrintLabel label) {
      printCommon(P, "pattern_expr", label);
      switch (P->getCachedMatchOperandOwnership()) {
      case ValueOwnership::Default:
        break;
      case ValueOwnership::Shared:
        printFieldRaw([](llvm::raw_ostream &os) { os << "borrowing"; },
                      "ownership");
        break;
      case ValueOwnership::InOut:
        printFieldRaw([](llvm::raw_ostream &os) { os << "mutating"; },
                      "ownership");
        break;
      case ValueOwnership::Owned:
        printFieldRaw([](llvm::raw_ostream &os) { os << "consuming"; },
                      "ownership");
        break;
      }
      if (auto m = P->getCachedMatchExpr())
        printRec(m, PrintLabel::suppressable("match_expr"));
      else
        printRec(P->getSubExpr(), PrintLabel::suppressable("sub_expr"));
      printFoot();
    }
    void visitBindingPattern(BindingPattern *P, PrintLabel label) {
      printCommon(P, "pattern_binding", label);
      printField(P->getIntroducerStringRef(), "kind");
      printRec(P->getSubPattern(), PrintLabel::suppressable("sub_pattern"));
      printFoot();
    }
    void visitEnumElementPattern(EnumElementPattern *P, PrintLabel label) {
      printCommon(P, "pattern_enum_element", label);

      printFieldQuotedRaw([&](raw_ostream &OS) {
        P->getParentType().print(PrintWithColorRAII(OS, TypeColor).getOS());
        OS << '.';
        PrintWithColorRAII(OS, IdentifierColor) << P->getName();
      }, "element");

      if (P->hasSubPattern()) {
        printRec(P->getSubPattern(), PrintLabel::suppressable("sub_pattern"));
      }
      printFoot();
    }
    void visitOptionalSomePattern(OptionalSomePattern *P, PrintLabel label) {
      printCommon(P, "pattern_optional_some", label);
      printRec(P->getSubPattern(), PrintLabel::suppressable("sub_pattern"));
      printFoot();
    }
    void visitBoolPattern(BoolPattern *P, PrintLabel label) {
      printCommon(P, "pattern_bool", label);
      printField(P->getValue(), "value");
      printFoot();
    }

  };

  /// PrintDecl - Visitor implementation of Decl::print.
  class PrintDecl : public DeclVisitor<PrintDecl, void, PrintLabel>,
                    public PrintBase {
  public:
    using PrintBase::PrintBase;

  private:
    void printWhereRequirements(
        PointerUnion<const AssociatedTypeDecl *, const GenericContext *> Owner
                                ) {
      const auto printWhere = [&](const TrailingWhereClause *Where) {
        if (Where) {
          printFieldQuotedRaw([&](raw_ostream &OS) {
            Where->print(OS, /*printWhereKeyword*/ false);
          }, "where_requirements");
        }
      };

      if (const auto GC = Owner.dyn_cast<const GenericContext *>()) {
        printWhere(GC->getTrailingWhereClause());
      } else {
        const auto ATD = Owner.get<const AssociatedTypeDecl *>();
        printWhere(ATD->getTrailingWhereClause());
      }
    }

    void printCommon(Decl *D, const char *Name, PrintLabel Label,
                     TerminalColor Color = DeclColor) {
      printHead(Name, Color, Label);

      printFlag(D->isImplicit(), "implicit", DeclModifierColor);
      printFlag(D->isHoisted(), "hoisted", DeclModifierColor);

      if (auto implAttr = D->getAttrs().getAttribute<ObjCImplementationAttr>()) {
        PrintLabel label =
            implAttr->isEarlyAdopter() ? "objc_impl" : "clang_impl";
        if (implAttr->CategoryName.empty())
          printFlag(label);
        else
          printFieldQuoted(implAttr->CategoryName.str(), label);
      }

      printSourceRange(D->getSourceRange(), &D->getASTContext());
      printFlag(D->TrailingSemiLoc.isValid(), "trailing_semi",
                DeclModifierColor);
    }

    void printInherited(InheritedTypes Inherited) {
      if (Inherited.empty())
        return;
      printFieldQuotedRaw([&](raw_ostream &OS) {
        interleave(Inherited.getEntries(), OS,
                   [&](InheritedEntry Super) { Super.getType().print(OS); },
                   ", ");
      }, "inherits");
    }

  public:
    void visitImportDecl(ImportDecl *ID, PrintLabel label) {
      printCommon(ID, "import_decl", label);

      printFlag(ID->isExported(), "exported");
      if (ID->getImportKind() != ImportKind::Module)
        printField(ID->getImportKind(), "kind");

      printFieldQuotedRaw([&](raw_ostream &OS) {
        // Check if module aliasing was used for the given imported module; for
        // example, if '-module-alias Foo=Bar' was passed and this module has
        // 'import Foo', its corresponding real module name 'Bar' should be printed.
        ImportPath::Builder scratch;
        ID->getRealImportPath(scratch).print(OS);
      }, "module", IdentifierColor);

      printFoot();
    }

    void visitExtensionDecl(ExtensionDecl *ED, PrintLabel label) {
      printCommon(ED, "extension_decl", label, ExtensionColor);
      printFlag(!ED->hasBeenBound(), "unbound");
      printNameRaw([&](raw_ostream &OS) {
        if (ED->hasBeenBound())
          ED->getExtendedType().print(OS);
        else
          ED->getExtendedTypeRepr()->print(OS);
      });
      printCommonPost(ED);
    }

    void visitTypeAliasDecl(TypeAliasDecl *TAD, PrintLabel label) {
      printCommon(TAD, "typealias", label);

      if (auto underlying = TAD->getCachedUnderlyingType()) {
        printFieldQuoted(underlying, "type", TypeColor);
      } else {
        printFlag("unresolved_type", TypeColor);
      }
      printWhereRequirements(TAD);
      printAttributes(TAD);

      printFoot();
    }

    void visitOpaqueTypeDecl(OpaqueTypeDecl *OTD, PrintLabel label) {
      printCommon(OTD, "opaque_type", label);

      printDeclName(OTD->getNamingDecl(), "naming_decl");
      printFieldQuotedRaw([&](raw_ostream &OS) {
        OS << OTD->getDeclaredInterfaceType() << " in "
           << OTD->getOpaqueInterfaceGenericSignature()->getAsString();

      }, "opaque_interface", TypeColor);
      printAttributes(OTD);

      printFoot();
    }

    void visitGenericTypeParamDecl(GenericTypeParamDecl *decl,
                                   PrintLabel label) {
      printCommon(decl, "generic_type_param", label);
      printField(decl->getDepth(), "depth");
      printField(decl->getIndex(), "index");

      switch (decl->getParamKind()) {
      case GenericTypeParamKind::Type:
        printField((StringRef)"type", "param_kind");
        break;
      case GenericTypeParamKind::Pack:
        printField((StringRef)"pack", "param_kind");
        break;
      case GenericTypeParamKind::Value:
        printField((StringRef)"value", "param_kind");
        break;
      }
      printAttributes(decl);

      printFoot();
    }

    void visitAssociatedTypeDecl(AssociatedTypeDecl *decl, PrintLabel label) {
      printCommon(decl, "associated_type_decl", label);

      StringRef fieldName("default");
      if (auto defaultDef = decl->getCachedDefaultDefinitionType()) {
        printFieldQuoted(*defaultDef, fieldName);
      } else {
        printField("<not computed>", fieldName);
      }

      printWhereRequirements(decl);
      if (decl->overriddenDeclsComputed()) {
        printFieldQuotedRaw([&](raw_ostream &OS) {
          interleave(decl->getOverriddenDecls(), OS,
                     [&](AssociatedTypeDecl *overridden) {
                       OS << overridden->getProtocol()->getName();
                     }, ", ");
        }, "overridden");
      }

      printAttributes(decl);
      printFoot();
    }

    void visitProtocolDecl(ProtocolDecl *PD, PrintLabel label) {
      printCommon(PD, "protocol", label);

      if (PD->isRequirementSignatureComputed()) {
        auto reqSig = PD->getRequirementSignature();

        std::string reqSigStr;
        llvm::raw_string_ostream out(reqSigStr);
        reqSig.print(PD, out);

        printFieldQuoted(out.str(), "requirement_signature");
      } else {
        printFlag("uncomputed_requirement_signature");
      }

      printCommonPost(PD);
    }

    void printGenericParameters(GenericParamList *Params) {
      if (!Params)
        return;

      printFieldQuotedRaw([&](raw_ostream &OS) {
        Params->print(OS);
      }, PrintLabel::suppressable("generic_params"), TypeColor);
    }

    void printAttributes(const Decl *D) {
      ASTContext *Ctx = &D->getASTContext();
      printList("attrs", D->getAttrs(), [&](auto *attr) {
        printRec(attr, Ctx);
      });
    }

    void printCommon(ValueDecl *VD, const char *Name, PrintLabel Label,
                     TerminalColor Color = DeclColor) {
      printCommon((Decl*)VD, Name, Label, Color);

      printDeclName(VD);
      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(VD))
        printGenericParameters(AFD->getParsedGenericParams());
      if (auto *GTD = dyn_cast<GenericTypeDecl>(VD))
        printGenericParameters(GTD->getParsedGenericParams());
      if (auto *MD = dyn_cast<MacroDecl>(VD))
        printGenericParameters(MD->getParsedGenericParams());

      if (VD->hasInterfaceType()) {
        printFieldQuoted(VD->getInterfaceType(), "interface type",
                         InterfaceTypeColor);
      }

      if (VD->hasAccess()) {
        printField(VD->getFormalAccess(), "access", AccessLevelColor);
      }

      if (VD->overriddenDeclsComputed()) {
        auto overridden = VD->getOverriddenDecls();
        if (!overridden.empty()) {
          printFieldQuotedRaw([&](raw_ostream &OS) {
            interleave(overridden, OS,
                       [&](ValueDecl *overridden) {
                         overridden->dumpRef(OS);
                       }, ", ");
          }, "override", OverrideColor);
        }
      }

      auto VarD = dyn_cast<VarDecl>(VD);
      const auto &attrs = VD->getAttrs();
      printFlag(attrs.hasAttribute<FinalAttr>() && !(VarD && VarD->isLet()),
                "final");
      printFlag(attrs.hasAttribute<ObjCAttr>(), "@objc");
      printFlag(attrs.hasAttribute<DynamicAttr>(), "dynamic");
      if (auto *attr = attrs.getAttribute<DynamicReplacementAttr>()) {
        printFlagRaw([&](raw_ostream &OS) {
          OS << "@_dynamicReplacement(for: \"";
          OS << attr->getReplacedFunctionName();
          OS << "\")";
        });
      }
      auto lifetimeString = getDumpString(VD->getLifetimeAnnotation());
      if (!lifetimeString.empty())
        printFlag(lifetimeString);
    }

    void printCommon(NominalTypeDecl *NTD, const char *Name, PrintLabel Label,
                     TerminalColor Color = DeclColor) {
      printCommon((ValueDecl *)NTD, Name, Label, Color);

      if (NTD->hasInterfaceType())
        printFlag(NTD->isResilient() ? "resilient" : "non_resilient");
    }

    void printCommonPost(const IterableDeclContext *IDC) {
      switch (IDC->getIterableContextKind()) {
      case IterableDeclContextKind::NominalTypeDecl: {
        const auto NTD = cast<NominalTypeDecl>(IDC);
        printInherited(NTD->getInherited());
        printWhereRequirements(NTD);
        break;
      }
      case IterableDeclContextKind::ExtensionDecl:
        const auto ED = cast<ExtensionDecl>(IDC);
        printInherited(ED->getInherited());
        printWhereRequirements(ED);
        break;
      }

      printAttributes(IDC->getDecl());

      auto members = ParseIfNeeded ? IDC->getMembers()
                                   : IDC->getCurrentMembersWithoutLoading();
      printList("members", members, [&](Decl *D) {
        printRec(D);
      });
      printFoot();
    }

    void visitSourceFile(const SourceFile &SF) {
      printHead("source_file", ASTNodeColor);
      printNameRaw([&](raw_ostream &OS) {
        OS << SF.getFilename();
      });

      auto items =
          ParseIfNeeded ? SF.getTopLevelItems() : SF.getCachedTopLevelItems();
      if (items) {
        printList("items", *items, [&](auto item) {
          if (item.isImplicit())
            return;

          if (auto decl = item.template dyn_cast<Decl *>()) {
            printRec(decl);
          } else if (auto stmt = item.template dyn_cast<Stmt *>()) {
            printRec(stmt, &SF.getASTContext());
          } else {
            auto expr = item.template get<Expr *>();
            printRec(expr);
          }
        });
      }
      printFoot();
    }

    void visitVarDecl(VarDecl *VD, PrintLabel label) {
      printCommon(VD, "var_decl", label);

      printFlag(VD->isDistributed(), "distributed", DeclModifierColor);
      printFlag(VD->isLet(), "let", DeclModifierColor);
      printFlag(VD->getAttrs().hasAttribute<LazyAttr>(), "lazy",
                DeclModifierColor);
      printStorageImpl(VD);
      printFlag(VD->getAttrs().hasAttribute<KnownToBeLocalAttr>(),
                "known_to_be_local", DeclModifierColor);
      if (auto *nonisolatedAttr =
              VD->getAttrs().getAttribute<NonisolatedAttr>()) {
        if (nonisolatedAttr->isUnsafe()) {
          printFlag(true, "nonisolated(unsafe)", DeclModifierColor);
        } else {
          printFlag(true, "nonisolated", DeclModifierColor);
        }
      }

      printAttributes(VD);
      printAccessors(VD);

      printFoot();
    }

    void printStorageImpl(AbstractStorageDecl *D) {
      printFlag(D->isStatic(), "type", DeclModifierColor);

      if (D->hasInterfaceType()) {
        auto impl = D->getImplInfo();
        printField(impl.getReadImpl(), "readImpl", DeclModifierColor);
        if (!impl.supportsMutation()) {
          printFlag("immutable", DeclModifierColor);
        } else {
          printField(impl.getWriteImpl(), "writeImpl", DeclModifierColor);
          printField(impl.getReadWriteImpl(), "readWriteImpl",
                     DeclModifierColor);
        }
      }
    }

    void printAccessors(AbstractStorageDecl *D) {
      printList("accessors", D->getAllAccessors(), [&](auto accessor) {
        printRec(accessor);
      });
    }

    void visitParamDecl(ParamDecl *PD, PrintLabel label) {
      printHead("parameter", ParameterColor, label);

      printDeclName(PD);
      if (!PD->getArgumentName().empty())
        printFieldQuoted(PD->getArgumentName(), "apiName", IdentifierColor);
      if (PD->hasInterfaceType()) {
        printFieldQuoted(PD->getInterfaceType(), "interface type",
                         InterfaceTypeColor);
      }

      if (auto specifier = PD->getCachedSpecifier()) {
        if (*specifier != ParamDecl::Specifier::Default) {
          printFlag(ParamDecl::getSpecifierSpelling(*specifier));
        }
      }

      if (PD->hasInterfaceType())
        printFlag(PD->isVariadic(), "variadic");
      printFlag(PD->isAutoClosure(), "autoclosure");
      printFlag(PD->getAttrs().hasAttribute<NonEphemeralAttr>(),"nonEphemeral");

      auto lifetimeString =
          getDumpString(PD->getLifetimeAnnotationFromAttributes());
      if (!lifetimeString.empty())
        printFlag(lifetimeString);

      printFlag(PD->isNoImplicitCopy(), "noImplicitCopy");

      if (PD->getDefaultArgumentKind() != DefaultArgumentKind::None) {
        printField(PD->getDefaultArgumentKind(), "default_arg");
      }
      if (PD->hasDefaultExpr() &&
          PD->getCachedDefaultArgumentCaptureInfo() &&
          !PD->getCachedDefaultArgumentCaptureInfo()->isTrivial()) {
        printFieldRaw([&](raw_ostream &OS) {
          PD->getCachedDefaultArgumentCaptureInfo()->print(OS);
        }, "", CapturesColor);
      }
      
      printFlag(PD->getAttrs().hasAttribute<KnownToBeLocalAttr>(),
                "known_to_be_local", DeclModifierColor);

      printAttributes(PD);

      if (auto init = PD->getStructuralDefaultExpr()) {
        printRec(init, "expression");
      }

      printFoot();
    }

    void visitParameterList(ParameterList *PL, PrintLabel label) {
      PrintBase::visitParameterList(PL, /*ctx=*/nullptr, label);
    }

    void visitEnumCaseDecl(EnumCaseDecl *ECD, PrintLabel label) {
      printCommon(ECD, "enum_case_decl", label);
      printList("elements", ECD->getElements(), [&](EnumElementDecl *D) {
        printRec(D);
      });
      printFoot();
    }

    void visitEnumDecl(EnumDecl *ED, PrintLabel label) {
      printCommon(ED, "enum_decl", label);
      printCommonPost(ED);
    }

    void visitEnumElementDecl(EnumElementDecl *EED, PrintLabel label) {
      printCommon(EED, "enum_element_decl", label);
      if (auto *paramList = EED->getParameterList()) {
        printRec(paramList, &EED->getASTContext(),
                 PrintLabel::suppressable("parameter_list"));
      }
      printAttributes(EED);
      printFoot();
    }

    void visitStructDecl(StructDecl *SD, PrintLabel label) {
      printCommon(SD, "struct_decl", label);
      printCommonPost(SD);
    }

    void visitClassDecl(ClassDecl *CD, PrintLabel label) {
      printCommon(CD, "class_decl", label);

      printFlag(CD->isExplicitDistributedActor(), "distributed");
      printFlag(CD->isExplicitActor(), "actor");
      printFlag(CD->getAttrs().hasAttribute<StaticInitializeObjCMetadataAttr>(),
                "@_staticInitializeObjCMetadata");

      printCommonPost(CD);
    }

    void visitBuiltinTupleDecl(BuiltinTupleDecl *BTD, PrintLabel label) {
      printCommon(BTD, "builtin_tuple_decl", label);
      printCommonPost(BTD);
    }

    void visitPatternBindingDecl(PatternBindingDecl *PBD, PrintLabel label) {
      printCommon(PBD, "pattern_binding_decl", label);
      printAttributes(PBD);

      printList(
          "pattern_entries", range(PBD->getNumPatternEntries()), [&](auto idx) {
            // Ensure that we have an object structure printed in parsable modes
            // so that the children aren't directly rendered as array elements.
            if (Writer.isParsable())
              printHead("pattern_entry", FieldLabelColor);

            printRec(PBD->getPattern(idx), PrintLabel::suppressable("pattern"));
            if (PBD->getOriginalInit(idx)) {
              printRec(PBD->getOriginalInit(idx), "original_init");
            }
            if (PBD->getInit(idx)) {
              printRec(PBD->getInit(idx), "processed_init");
            }

            if (Writer.isParsable())
              printFoot();
          });
      printFoot();
    }

    void visitSubscriptDecl(SubscriptDecl *SD, PrintLabel label) {
      printCommon(SD, "subscript_decl", label);
      printStorageImpl(SD);
      printAttributes(SD);
      printAccessors(SD);
      printFoot();
    }

    void printCommonAFD(AbstractFunctionDecl *D, const char *Type,
                        PrintLabel Label) {
      printCommon(D, Type, Label, FuncColor);
      if (auto captureInfo = D->getCachedCaptureInfo()) {
        if (!captureInfo->isTrivial()) {
          printFieldRaw([&](raw_ostream &OS) {
            captureInfo->print(OS);
          }, PrintLabel::suppressable("captures"));
        }
      }

      if (auto *attr = D->getAttrs().getAttribute<NonisolatedAttr>()) {
        printFlag(attr->isUnsafe() ? "nonisolated(unsafe)" : "nonisolated",
                  ExprModifierColor);
      }
      printFlag(D->isDistributed(), "distributed", ExprModifierColor);
      printFlag(D->isDistributedThunk(), "distributed_thunk",ExprModifierColor);
    }

    void printAbstractFunctionDecl(AbstractFunctionDecl *D) {
      printAttributes(D);
      if (auto *P = D->getImplicitSelfDecl()) {
        printRec(P, PrintLabel::suppressable("implicit_self_decl"));
      }
      printRec(D->getParameters(), &D->getASTContext(),
               PrintLabel::suppressable("parameters"));

      if (auto FD = dyn_cast<FuncDecl>(D)) {
        if (FD->getResultTypeRepr()) {
          printRec(FD->getResultTypeRepr(), "result");
          if (auto opaque = FD->getOpaqueResultTypeDecl()) {
            printRec(opaque, "opaque_result_decl");
          }
        }
      }

      if (auto thrownTypeRepr = D->getThrownTypeRepr()) {
        printRec(thrownTypeRepr, "thrown_type");
      }

      if (auto fac = D->getForeignAsyncConvention()) {
        printRecArbitrary([&](PrintLabel label) {
          printHead("foreign_async_convention", ASTNodeColor, label);
          if (auto type = fac->completionHandlerType())
            printFieldQuoted(type, "completion_handler_type", TypeColor);
          printField(fac->completionHandlerParamIndex(),
                     "completion_handler_param");
          if (auto errorParamIndex = fac->completionHandlerErrorParamIndex())
            printField(*errorParamIndex, "error_param");
          printFoot();
        });
      }

      if (auto fec = D->getForeignErrorConvention()) {
        printRecArbitrary([&](PrintLabel label) {
          printHead("foreign_error_convention", ASTNodeColor, label);
          printField(fec->getKind(), "kind");

          bool wantResultType = (
            fec->getKind() == ForeignErrorConvention::ZeroResult ||
            fec->getKind() == ForeignErrorConvention::NonZeroResult);

          printFlag(getDumpString(fec->isErrorOwned()));

          printField(fec->getErrorParameterIndex(), "param");
          printFieldQuoted(fec->getErrorParameterType(), "paramtype");
          if (wantResultType)
            printFieldQuoted(fec->getResultType(), "resulttype");
          printFoot();
        });
      }

      auto canParse = ParseIfNeeded && !D->isBodySkipped();
      if (auto Body = D->getBody(canParse)) {
        printRec(Body, &D->getASTContext(), PrintLabel::suppressable("body"));
      }
    }

    void printCommonFD(FuncDecl *FD, const char *type, PrintLabel Label) {
      printCommonAFD(FD, type, Label);
      printFlag(FD->isStatic(), "type");
    }

    void visitFuncDecl(FuncDecl *FD, PrintLabel label) {
      printCommonFD(FD, "func_decl", label);
      printAbstractFunctionDecl(FD);
      printFoot();
    }

    void visitAccessorDecl(AccessorDecl *AD, PrintLabel label) {
      printCommonFD(AD, "accessor_decl", label);
      printFlag(getDumpString(AD->getAccessorKind()));
      printDeclName(AD->getStorage(), "for");
      printAbstractFunctionDecl(AD);
      printFoot();
    }

    void visitConstructorDecl(ConstructorDecl *CD, PrintLabel label) {
      printCommonAFD(CD, "constructor_decl", label);
      printFlag(CD->isRequired(), "required", DeclModifierColor);
      printFlag(getDumpString(CD->getInitKind()), DeclModifierColor);
      if (CD->isFailable())
        printField((CD->isImplicitlyUnwrappedOptional()
                         ? "ImplicitlyUnwrappedOptional"
                         : "Optional"), "failable", DeclModifierColor);
      printAbstractFunctionDecl(CD);
      printFoot();
    }

    void visitDestructorDecl(DestructorDecl *DD, PrintLabel label) {
      printCommonAFD(DD, "destructor_decl", label);
      printAbstractFunctionDecl(DD);
      printFoot();
    }

    void visitTopLevelCodeDecl(TopLevelCodeDecl *TLCD, PrintLabel label) {
      printCommon(TLCD, "top_level_code_decl", label);
      if (TLCD->getBody()) {
        printRec(TLCD->getBody(), &static_cast<Decl *>(TLCD)->getASTContext(),
                 PrintLabel::suppressable("body"));
      }
      printFoot();
    }

    void visitPoundDiagnosticDecl(PoundDiagnosticDecl *PDD, PrintLabel label) {
      printCommon(PDD, "pound_diagnostic_decl", label);
      printField(PDD->isError() ? "error" : "warning", "kind");
      printRec(PDD->getMessage(), PrintLabel::suppressable("message"));
      printFoot();
    }

    void visitPrecedenceGroupDecl(PrecedenceGroupDecl *PGD, PrintLabel label) {
      printCommon(PGD, "precedence_group_decl", label);
      printName(PGD->getName());
      printField(PGD->getAssociativity(), "associativity");
      printField(PGD->isAssignment(), "assignment");

      auto printRelationsRec =
          [&](ArrayRef<PrecedenceGroupDecl::Relation> rels, StringRef name) {
        if (rels.empty()) return;

        // The S-expression representation doesn't render cleanly in JSON
        // because it uses "flags" to just write out the names of the groups
        // in order. Impose a little more structure for JSON.
        if (Writer.isParsable()) {
          printList(name, rels, [&](auto &rel) {
            printRecArbitrary([&](PrintLabel label) {
              printHead(rel.Name.str(), FieldLabelColor, label);
              printFoot();
            }, "name");
          });
          return;
        }

        printRecArbitrary([&](PrintLabel label) {
          printHead(name, FieldLabelColor, label);
          for (auto &rel : rels)
            printFlag(rel.Name.str());
          printFoot();
        });
      };
      printRelationsRec(PGD->getHigherThan(), "higherThan");
      printRelationsRec(PGD->getLowerThan(), "lowerThan");

      printFoot();
    }

    void visitInfixOperatorDecl(InfixOperatorDecl *IOD, PrintLabel label) {
      printCommon(IOD, "infix_operator_decl", label);
      printName(IOD->getName());
      if (!IOD->getPrecedenceGroupName().empty())
        printFieldQuoted(IOD->getPrecedenceGroupName(),
                         "precedence_group_name");
      printFoot();
    }

    void visitPrefixOperatorDecl(PrefixOperatorDecl *POD, PrintLabel label) {
      printCommon(POD, "prefix_operator_decl", label);
      printName(POD->getName());
      printFoot();
    }

    void visitPostfixOperatorDecl(PostfixOperatorDecl *POD, PrintLabel label) {
      printCommon(POD, "postfix_operator_decl", label);
      printName(POD->getName());
      printFoot();
    }

    void visitModuleDecl(ModuleDecl *MD, PrintLabel label) {
      printCommon(MD, "module", label);
      printFlag(MD->isNonSwiftModule(), "non_swift");
      printAttributes(MD);
      printFoot();
    }

    void visitMissingDecl(MissingDecl *missing, PrintLabel label) {
      printCommon(missing, "missing_decl", label);
      printFoot();
    }

    void visitMissingMemberDecl(MissingMemberDecl *MMD, PrintLabel label) {
      printCommon(MMD, "missing_member_decl ", label);
      printName(MMD->getName());
      printFoot();
    }

    void visitMacroDecl(MacroDecl *MD, PrintLabel label) {
      printCommon(MD, "macro_decl", label);
      printAttributes(MD);
      printRec(MD->getParameterList(), &MD->getASTContext(),
               PrintLabel::suppressable("parameters"));
      if (MD->resultType.getTypeRepr())
        printRec(MD->resultType.getTypeRepr(), "result");
      printRec(MD->definition, "definition");
      printFoot();
    }

    void visitMacroExpansionDecl(MacroExpansionDecl *MED, PrintLabel label) {
      printCommon(MED, "macro_expansion_decl", label);
      printName(MED->getMacroName().getFullName());
      printRec(MED->getArgs(), PrintLabel::suppressable("args"));
      printFoot();
    }
  };
} // end anonymous namespace

void ParameterList::dump() const {
  dump(llvm::errs(), 0);
  llvm::errs() << '\n';
}

void ParameterList::dump(raw_ostream &OS, unsigned Indent) const {
  DefaultWriter writer(OS, Indent);
  PrintDecl(writer).visitParameterList(const_cast<ParameterList *>(this), "");
}

void Decl::dump() const {
  dump(llvm::errs(), 0);
}

void Decl::dump(const char *filename) const {
  std::error_code ec;
  llvm::raw_fd_ostream stream(filename, ec, llvm::sys::fs::FA_Read |
                              llvm::sys::fs::FA_Write);
  // In assert builds, we blow up. Otherwise, we just return.
  assert(!ec && "Failed to open file for dumping?!");
  if (ec)
    return;
  dump(stream, 0);
}

void Decl::dump(raw_ostream &OS, unsigned Indent) const {
  DefaultWriter writer(OS, Indent);
  PrintDecl(writer).visit(const_cast<Decl *>(this), "");
  OS << '\n';
}

/// Print the given declaration context (with its parents).
void swift::printContext(raw_ostream &os, DeclContext *dc) {
  if (auto parent = dc->getParent()) {
    printContext(os, parent);
    os << '.';
  }

  switch (dc->getContextKind()) {
  case DeclContextKind::Package:
    printName(os, cast<PackageUnit>(dc)->getName());
    break;

  case DeclContextKind::Module:
    printName(os, cast<ModuleDecl>(dc)->getRealName());
    break;

  case DeclContextKind::FileUnit:
    // FIXME: print the file's basename?
    os << "(file)";
    break;

  case DeclContextKind::SerializedAbstractClosure:
    os << "serialized abstract closure";
    break;

  case DeclContextKind::AbstractClosureExpr: {
    auto *ACE = cast<AbstractClosureExpr>(dc);
    if (isa<ClosureExpr>(ACE)) {
      PrintWithColorRAII(os, DiscriminatorColor)
        << "explicit closure discriminator=";
    }
    if (isa<AutoClosureExpr>(ACE)) {
      PrintWithColorRAII(os, DiscriminatorColor)
        << "autoclosure discriminator=";
    }

    // If we aren't printing to standard error or the debugger output stream,
    // this client expects to see the computed discriminator. Compute it now.
    if (&os != &llvm::errs() && &os != &llvm::dbgs())
      (void)ACE->getDiscriminator();

    PrintWithColorRAII(os, DiscriminatorColor) << ACE->getRawDiscriminator();
    break;
  }

  case DeclContextKind::GenericTypeDecl:
    printName(os, cast<GenericTypeDecl>(dc)->getName());
    break;

  case DeclContextKind::ExtensionDecl:
    if (auto extendedNominal = cast<ExtensionDecl>(dc)->getExtendedNominal()) {
      printName(os, extendedNominal->getName());
    }
    os << " extension";
    break;

  case DeclContextKind::Initializer:
    simple_display(os, cast<Initializer>(dc));
    break;

  case DeclContextKind::TopLevelCodeDecl:
  case DeclContextKind::SerializedTopLevelCodeDecl:
    os << "top-level code";
    break;

  case DeclContextKind::AbstractFunctionDecl:
    printName(os, cast<AbstractFunctionDecl>(dc)->getName());
    break;

  case DeclContextKind::SubscriptDecl:
    printName(os, cast<SubscriptDecl>(dc)->getName());
    break;

  case DeclContextKind::EnumElementDecl:
    printName(os, cast<EnumElementDecl>(dc)->getName());
    break;

  case DeclContextKind::MacroDecl:
    printName(os, cast<MacroDecl>(dc)->getName());
    break;
  }
}

std::string ValueDecl::printRef() const {
  std::string result;
  llvm::raw_string_ostream os(result);
  dumpRef(os);
  return os.str();
}

void ValueDecl::dumpRef(raw_ostream &os) const {
  if (!isa<ModuleDecl>(this)) {
    // Print the context.
    printContext(os, getDeclContext());
    os << ".";
    // Print name.
    getName().printPretty(os);
  } else {
    auto moduleName = cast<ModuleDecl>(this)->getRealName();
    os << moduleName;
  }

  if (getAttrs().hasAttribute<KnownToBeLocalAttr>()) {
    os << " known-to-be-local";
  }

  // Print location.
  auto &srcMgr = getASTContext().SourceMgr;
  if (getLoc().isValid()) {
    os << '@';
    getLoc().print(os, srcMgr);
  }
}

void LLVM_ATTRIBUTE_USED ValueDecl::dumpRef() const {
  dumpRef(llvm::errs());
  llvm::errs() << "\n";
}

void SourceFile::dump() const {
  dump(llvm::errs());
}

void SourceFile::dump(llvm::raw_ostream &OS, bool parseIfNeeded) const {
  DefaultWriter writer(OS, /*indent*/ 0);
  PrintDecl(writer, parseIfNeeded).visitSourceFile(*this);
  llvm::errs() << '\n';
}

void SourceFile::dumpJSON(llvm::raw_ostream &OS) const {
  JSONWriter writer(OS, /*indent*/ 2);
  PrintDecl(writer, /*parseIfNeeded*/ true).visitSourceFile(*this);
}

void Pattern::dump() const {
  dump(llvm::errs());
}

void Pattern::dump(raw_ostream &OS, unsigned Indent) const {
  DefaultWriter writer(OS, Indent);
  PrintPattern(writer).visit(const_cast<Pattern*>(this), "");
  OS << '\n';
}

//===----------------------------------------------------------------------===//
// Printing for Stmt and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
/// PrintStmt - Visitor implementation of Stmt::dump.
class PrintStmt : public StmtVisitor<PrintStmt, void, PrintLabel>,
                  public PrintBase {
public:
  using PrintBase::PrintBase;
  const ASTContext *Ctx;

  PrintStmt(
      PrintWriterBase &writer, const ASTContext *ctx,
      bool parseIfNeeded = false,
      llvm::function_ref<Type(Expr *)> getTypeOfExpr = defaultGetTypeOfExpr,
      llvm::function_ref<Type(TypeRepr *)> getTypeOfTypeRepr = nullptr,
      llvm::function_ref<Type(KeyPathExpr *E, unsigned index)>
          getTypeOfKeyPathComponent = defaultGetTypeOfKeyPathComponent)
      : PrintBase(writer, parseIfNeeded, getTypeOfExpr, getTypeOfTypeRepr,
                  getTypeOfKeyPathComponent),
        Ctx(ctx) {}

  using PrintBase::printRec;

  void printRec(Stmt *S, PrintLabel Label = "") {
    PrintBase::printRec(S, Ctx, Label);
  }

  void printCommon(Stmt *S, const char *Name, PrintLabel Label) {
    printHead(Name, StmtColor, Label);

    printFlag(S->isImplicit(), "implicit");
    printSourceRange(S->getSourceRange(), Ctx);
    printFlag(S->TrailingSemiLoc.isValid(), "trailing_semi");
  }

  void visitBraceStmt(BraceStmt *S, PrintLabel label) {
    printCommon(S, "brace_stmt", label);
    printList("elements", S->getElements(), [&](auto &Elt) {
      printRec(Elt, Ctx);
    });
    printFoot();
  }

  void visitReturnStmt(ReturnStmt *S, PrintLabel label) {
    printCommon(S, "return_stmt", label);
    if (S->hasResult()) {
      printRec(S->getResult(), PrintLabel::suppressable("result"));
    }
    printFoot();
  }

  void visitYieldStmt(YieldStmt *S, PrintLabel label) {
    printCommon(S, "yield_stmt", label);
    printList("yields", S->getYields(), [&](auto yield) {
      printRec(yield);
    });
    printFoot();
  }

  void visitThenStmt(ThenStmt *S, PrintLabel label) {
    printCommon(S, "then_stmt", label);
    printRec(S->getResult(), PrintLabel::suppressable("result"));
    printFoot();
  }

  void visitDeferStmt(DeferStmt *S, PrintLabel label) {
    printCommon(S, "defer_stmt", label);
    printRec(S->getTempDecl(), PrintLabel::suppressable("temp_decl"));
    printRec(S->getCallExpr(), PrintLabel::suppressable("call_expr"));
    printFoot();
  }

  void visitIfStmt(IfStmt *S, PrintLabel label) {
    printCommon(S, "if_stmt", label);
    printRecRange(S->getCond(), Ctx, "conditions");
    printRec(S->getThenStmt(), PrintLabel::suppressable("then"));
    if (S->getElseStmt()) {
      printRec(S->getElseStmt(), PrintLabel::suppressable("else"));
    }
    printFoot();
  }

  void visitGuardStmt(GuardStmt *S, PrintLabel label) {
    printCommon(S, "guard_stmt", label);
    printRecRange(S->getCond(), Ctx, "conditions");
    printRec(S->getBody(), PrintLabel::suppressable("body"));
    printFoot();
  }

  void visitDoStmt(DoStmt *S, PrintLabel label) {
    printCommon(S, "do_stmt", label);
    printRec(S->getBody(), PrintLabel::suppressable("body"));
    printFoot();
  }

  void visitWhileStmt(WhileStmt *S, PrintLabel label) {
    printCommon(S, "while_stmt", label);
    printRecRange(S->getCond(), Ctx, "conditions");
    printRec(S->getBody(), PrintLabel::suppressable("body"));
    printFoot();
  }

  void visitRepeatWhileStmt(RepeatWhileStmt *S, PrintLabel label) {
    printCommon(S, "repeat_while_stmt", label);
    printRec(S->getBody(), PrintLabel::suppressable("body"));
    printRec(S->getCond(), PrintLabel::suppressable("cond"));
    printFoot();
  }
  void visitForEachStmt(ForEachStmt *S, PrintLabel label) {
    printCommon(S, "for_each_stmt", label);
    printRec(S->getPattern(), PrintLabel::suppressable("pattern"));
    if (S->getWhere()) {
      printRec(S->getWhere(), "where");
    }
    printRec(S->getParsedSequence(),
             PrintLabel::suppressable("parsed_sequence"));
    if (S->getIteratorVar()) {
      printRec(S->getIteratorVar(), PrintLabel::suppressable("iterator_var"));
    }
    if (S->getNextCall()) {
      printRec(S->getNextCall(), PrintLabel::suppressable("next_call"));
    }
    if (S->getConvertElementExpr()) {
      printRec(S->getConvertElementExpr(),
               PrintLabel::suppressable("convert_element_expr"));
    }
    if (S->getElementExpr()) {
      printRec(S->getElementExpr(), PrintLabel::suppressable("element_expr"));
    }
    printRec(S->getBody(), PrintLabel::suppressable("body"));
    printFoot();
  }
  void visitBreakStmt(BreakStmt *S, PrintLabel label) {
    printCommon(S, "break_stmt", label);
    printFoot();
  }
  void visitContinueStmt(ContinueStmt *S, PrintLabel label) {
    printCommon(S, "continue_stmt", label);
    printFoot();
  }
  void visitFallthroughStmt(FallthroughStmt *S, PrintLabel label) {
    printCommon(S, "fallthrough_stmt", label);
    printFoot();
  }
  void visitSwitchStmt(SwitchStmt *S, PrintLabel label) {
    printCommon(S, "switch_stmt", label);
    printRec(S->getSubjectExpr(), PrintLabel::suppressable("subject_expr"));
    printList("cases", S->getRawCases(), [&](auto N) {
      if (N.template is<Stmt*>())
        printRec(N.template get<Stmt*>());
      else
        printRec(N.template get<Decl*>());
    });
    printFoot();
  }
  void visitCaseStmt(CaseStmt *S, PrintLabel label) {
    printCommon(S, "case_stmt", label);
    printFlag(S->hasUnknownAttr(), "@unknown");

    if (S->hasCaseBodyVariables()) {
      printRecRange(S->getCaseBodyVariables(), "case_body_variables");
    }

    printList(
        "case_label_items", S->getCaseLabelItems(), [&](const auto &LabelItem) {
          printRecArbitrary([&](PrintLabel label) {
            printHead("case_label_item", StmtColor, label);
            printFlag(LabelItem.isDefault(), "default");

            if (auto *CasePattern = LabelItem.getPattern()) {
              switch (CasePattern->getOwnership()) {
              case ValueOwnership::Default:
                break;
              case ValueOwnership::Shared:
                printFieldRaw([](llvm::raw_ostream &os) { os << "borrowing"; },
                              "ownership");
                break;
              case ValueOwnership::InOut:
                printFieldRaw([](llvm::raw_ostream &os) { os << "mutating"; },
                              "ownership");
                break;
              case ValueOwnership::Owned:
                printFieldRaw([](llvm::raw_ostream &os) { os << "consuming"; },
                              "ownership");
                break;
              }
              printRec(CasePattern, PrintLabel::suppressable("pattern"));
            }
            if (auto *Guard = LabelItem.getGuardExpr()) {
              printRec(const_cast<Expr *>(Guard),
                       PrintLabel::suppressable("where"));
            }

            printFoot();
          });
        });

    printRec(S->getBody(), PrintLabel::suppressable("body"));
    printFoot();
  }
  void visitFailStmt(FailStmt *S, PrintLabel label) {
    printCommon(S, "fail_stmt", label);
    printFoot();
  }

  void visitThrowStmt(ThrowStmt *S, PrintLabel label) {
    printCommon(S, "throw_stmt", label);
    printRec(S->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }

  void visitDiscardStmt(DiscardStmt *S, PrintLabel label) {
    printCommon(S, "discard_stmt", label);
    printRec(S->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }

  void visitPoundAssertStmt(PoundAssertStmt *S, PrintLabel label) {
    printCommon(S, "pound_assert", label);
    printFieldQuoted(S->getMessage(), "message");
    printRec(S->getCondition(), PrintLabel::suppressable("condition"));
    printFoot();
  }

  void visitDoCatchStmt(DoCatchStmt *S, PrintLabel label) {
    printCommon(S, "do_catch_stmt", label);
    printThrowDest(S->rethrows(), /*wantNothrow=*/true);
    printRec(S->getBody(), "body");
    printRecRange(S->getCatches(), Ctx, "catch_stmts");
    printFoot();
  }
};

} // end anonymous namespace

void Stmt::dump() const {
  dump(llvm::errs());
  llvm::errs() << '\n';
}

void Stmt::dump(raw_ostream &OS, const ASTContext *Ctx, unsigned Indent) const {
  DefaultWriter writer(OS, Indent);
  PrintStmt(writer, Ctx).visit(const_cast<Stmt*>(this), "");
}

//===----------------------------------------------------------------------===//
// Printing for Expr and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
/// PrintExpr - Visitor implementation of Expr::dump.
class PrintExpr : public ExprVisitor<PrintExpr, void, PrintLabel>,
                  public PrintBase {
public:
  using PrintBase::PrintBase;

  /// FIXME: This should use ExprWalker to print children.

  void printCommon(Expr *E, const char *C, PrintLabel label) {
    PrintOptions PO;
    PO.PrintTypesForDebugging = true;

    printHead(C, ExprColor, label);

    printFlag(E->isImplicit(), "implicit", ExprModifierColor);
    printFieldQuoted(GetTypeOfExpr(E).getString(PO), "type", TypeColor);

    // If we have a source range and an ASTContext, print the source range.
    if (auto Ty = GetTypeOfExpr(E)) {
      auto &Ctx = Ty->getASTContext();
      printSourceLoc(E->getLoc(), &Ctx);
      printSourceRange(E->getSourceRange(), &Ctx);
    }

    printFlag(E->TrailingSemiLoc.isValid(), "trailing_semi");
  }
  
  void printSemanticExpr(Expr * semanticExpr) {
    if (semanticExpr == nullptr) {
      return;
    }
    
    printRec(semanticExpr, "semantic_expr");
  }

  void visitErrorExpr(ErrorExpr *E, PrintLabel label) {
    printCommon(E, "error_expr", label);
    printFoot();
  }

  void visitCodeCompletionExpr(CodeCompletionExpr *E, PrintLabel label) {
    printCommon(E, "code_completion_expr", label);
    if (E->getBase()) {
      printRec(E->getBase(), PrintLabel::suppressable("base"));
    }
    printFoot();
  }

  void printInitializerField(ConcreteDeclRef declRef, PrintLabel label) {
    printFieldQuotedRaw([&](raw_ostream &OS) { declRef.dump(OS); }, label,
                  ExprModifierColor);
  }

  void visitNilLiteralExpr(NilLiteralExpr *E, PrintLabel label) {
    printCommon(E, "nil_literal_expr", label);
    printInitializerField(E->getInitializer(), "initializer");
    printFoot();
  }

  void visitIntegerLiteralExpr(IntegerLiteralExpr *E, PrintLabel label) {
    printCommon(E, "integer_literal_expr", label);
    
    printFlag(E->isNegative(), "negative", LiteralValueColor);
    Type T = GetTypeOfExpr(E);
    if (T.isNull() || !T->is<BuiltinIntegerType>())
      printFieldQuoted(E->getDigitsText(), "value", LiteralValueColor);
    else
      printFieldQuoted(E->getValue(), "value", LiteralValueColor);
    printInitializerField(E->getBuiltinInitializer(), "builtin_initializer");
    printInitializerField(E->getInitializer(), "initializer");

    printFoot();
  }
  void visitFloatLiteralExpr(FloatLiteralExpr *E, PrintLabel label) {
    printCommon(E, "float_literal_expr", label);
    
    printFlag(E->isNegative(), "negative", LiteralValueColor);
    printFieldQuoted(E->getDigitsText(), "value", LiteralValueColor);
    printInitializerField(E->getBuiltinInitializer(), "builtin_initializer");
    printInitializerField(E->getInitializer(), "initializer");
    if (!E->getBuiltinType().isNull()) {
      printFieldQuoted(E->getBuiltinType(), "builtin_type", ExprModifierColor);
    }
    
    printFoot();
  }

  void visitBooleanLiteralExpr(BooleanLiteralExpr *E, PrintLabel label) {
    printCommon(E, "boolean_literal_expr", label);
    
    printField(E->getValue(), "value", LiteralValueColor);
    printInitializerField(E->getBuiltinInitializer(), "builtin_initializer");
    printInitializerField(E->getInitializer(), "initializer");

    printFoot();
  }

  void visitStringLiteralExpr(StringLiteralExpr *E, PrintLabel label) {
    printCommon(E, "string_literal_expr", label);
    
    printField(E->getEncoding(), "encoding", ExprModifierColor);
    printFieldQuoted(E->getValue(), "value", LiteralValueColor);
    printInitializerField(E->getBuiltinInitializer(), "builtin_initializer");
    printInitializerField(E->getInitializer(), "initializer");

    printFoot();
  }
  void visitInterpolatedStringLiteralExpr(InterpolatedStringLiteralExpr *E,
                                          PrintLabel label) {
    printCommon(E, "interpolated_string_literal_expr", label);

    printField(E->getLiteralCapacity(), "literal_capacity", ExprModifierColor);
    printField(E->getInterpolationCount(), "interpolation_count",
               ExprModifierColor);
    printInitializerField(E->getBuilderInit(), "builder_init");
    printInitializerField(E->getInitializer(), "result_init");

    printRec(E->getAppendingExpr(), PrintLabel::suppressable("appending_expr"));

    printFoot();
  }
  void visitMagicIdentifierLiteralExpr(MagicIdentifierLiteralExpr *E,
                                       PrintLabel label) {
    printCommon(E, "magic_identifier_literal_expr", label);
    
    printField(E->getKind(), "kind", ExprModifierColor);

    if (E->isString()) {
      printField(E->getStringEncoding(), "encoding", ExprModifierColor);
    }
    printInitializerField(E->getBuiltinInitializer(), "builtin_initializer");
    printInitializerField(E->getInitializer(), "initializer");

    printFoot();
  }
  void visitRegexLiteralExpr(RegexLiteralExpr *E, PrintLabel label) {
    printCommon(E, "regex_literal_expr", label);

    printFieldQuoted(E->getParsedRegexText(), "text", LiteralValueColor);
    printInitializerField(E->getInitializer(), "initializer");

    printFoot();
  }

  void visitObjectLiteralExpr(ObjectLiteralExpr *E, PrintLabel label) {
    printCommon(E, "object_literal", label);

    printField(E->getLiteralKind(), "kind");
    printInitializerField(E->getInitializer(), "initializer");

    printRec(E->getArgs(), PrintLabel::suppressable("args"));
    
    printFoot();
  }

  void visitDiscardAssignmentExpr(DiscardAssignmentExpr *E, PrintLabel label) {
    printCommon(E, "discard_assignment_expr", label);
    printFoot();
  }

  void visitDeclRefExpr(DeclRefExpr *E, PrintLabel label) {
    printCommon(E, "declref_expr", label);
    printThrowDest(E->throws(), /*wantNothrow=*/false);

    printDeclRefField(E->getDeclRef(), "decl");
    if (E->getAccessSemantics() != AccessSemantics::Ordinary)
      printFlag(getDumpString(E->getAccessSemantics()), AccessLevelColor);
    printFieldRaw([&](auto &os) { E->getFunctionRefInfo().dump(os); },
                  "function_ref", ExprModifierColor);

    printFoot();
  }
  void visitSuperRefExpr(SuperRefExpr *E, PrintLabel label) {
    printCommon(E, "super_ref_expr", label);
    printFoot();
  }

  void visitTypeExpr(TypeExpr *E, PrintLabel label) {
    printCommon(E, "type_expr", label);

    if (E->getTypeRepr())
      printFieldQuotedRaw([&](raw_ostream &OS) { E->getTypeRepr()->print(OS); },
                          "typerepr", TypeReprColor);
    else
      printFlag("null_typerepr");

    printFoot();
  }

  void visitOtherConstructorDeclRefExpr(OtherConstructorDeclRefExpr *E,
                                        PrintLabel label) {
    printCommon(E, "other_constructor_ref_expr", label);
    printDeclRefField(E->getDeclRef(), "decl");
    printFoot();
  }
  void visitOverloadedDeclRefExpr(OverloadedDeclRefExpr *E, PrintLabel label) {
    printCommon(E, "overloaded_decl_ref_expr", label);

    printFieldQuoted(E->getDecls()[0]->getBaseName(), "name", IdentifierColor);
    printField(E->getDecls().size(), "number_of_decls", ExprModifierColor);
    printFieldRaw([&](auto &os) { E->getFunctionRefInfo().dump(os); },
                  "function_ref", ExprModifierColor);

    if (!E->isForOperator()) {
      printList("candidates", E->getDecls(), [&](auto D) {
        printRecArbitrary([&](PrintLabel label) {
          printHead("candidate_decl", DeclModifierColor, label);
          printNameRaw([&](raw_ostream &OS) { D->dumpRef(OS); });
          printFoot();
        });
      });
    }

    printFoot();
  }
  void visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *E, PrintLabel label) {
    printCommon(E, "unresolved_decl_ref_expr", label);

    printFieldQuoted(E->getName(), "name", IdentifierColor);
    printFieldRaw([&](auto &os) { E->getFunctionRefInfo().dump(os); },
                  "function_ref", ExprModifierColor);

    printFoot();
  }
  void visitUnresolvedSpecializeExpr(UnresolvedSpecializeExpr *E,
                                     PrintLabel label) {
    printCommon(E, "unresolved_specialize_expr", label);

    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printList("unresolved_params", E->getUnresolvedParams(), [&](TypeLoc T) {
      printRec(T.getTypeRepr(), PrintLabel::suppressable("type_repr"));
    });

    printFoot();
  }

  void visitMemberRefExpr(MemberRefExpr *E, PrintLabel label) {
    printCommon(E, "member_ref_expr", label);
    printThrowDest(E->throws(), /*wantNothrow=*/false);

    printDeclRefField(E->getMember(), "decl");
    if (E->getAccessSemantics() != AccessSemantics::Ordinary)
      printFlag(getDumpString(E->getAccessSemantics()), AccessLevelColor);
    printFlag(E->isSuper(), "super");

    printRec(E->getBase(), PrintLabel::suppressable("base"));
    printFoot();
  }
  void visitDynamicMemberRefExpr(DynamicMemberRefExpr *E, PrintLabel label) {
    printCommon(E, "dynamic_member_ref_expr", label);
    printThrowDest(E->throws(), /*wantNothrow=*/false);

    printDeclRefField(E->getMember(), "decl");

    printRec(E->getBase(), PrintLabel::suppressable("base"));

    printFoot();
  }
  void visitUnresolvedMemberExpr(UnresolvedMemberExpr *E, PrintLabel label) {
    printCommon(E, "unresolved_member_expr", label);

    printFieldQuoted(E->getName(), "name", ExprModifierColor);
    printFieldRaw([&](auto &os) { E->getFunctionRefInfo().dump(os); },
                  "function_ref", ExprModifierColor);
    printFoot();
  }
  void visitDotSelfExpr(DotSelfExpr *E, PrintLabel label) {
    printCommon(E, "dot_self_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitParenExpr(ParenExpr *E, PrintLabel label) {
    printCommon(E, "paren_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitAwaitExpr(AwaitExpr *E, PrintLabel label) {
    printCommon(E, "await_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitConsumeExpr(ConsumeExpr *E, PrintLabel label) {
    printCommon(E, "consume_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitCopyExpr(CopyExpr *E, PrintLabel label) {
    printCommon(E, "copy_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitBorrowExpr(BorrowExpr *E, PrintLabel label) {
    printCommon(E, "borrow_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitUnresolvedMemberChainResultExpr(UnresolvedMemberChainResultExpr *E,
                                            PrintLabel label) {
    printCommon(E, "unresolved_member_chain_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitTupleExpr(TupleExpr *E, PrintLabel label) {
    printCommon(E, "tuple_expr", label);

    if (E->hasElementNames()) {
      printFieldQuotedRaw([&](raw_ostream &OS) {
        interleave(E->getElementNames(), OS,
                   [&](Identifier name) {
                     OS << (name.empty()?"''":name.str());
                   },
                   ",");
      }, "names", IdentifierColor);
    }

    printList("elements", range(E->getNumElements()), [&](unsigned i) {
      if (E->getElement(i))
        printRec(E->getElement(i));
      else {
        printRecArbitrary([&](PrintLabel label) {
          printHead("<tuple element default value>", ExprColor);
          printFoot();
        });
      }
    });

    printFoot();
  }
  void visitArrayExpr(ArrayExpr *E, PrintLabel label) {
    printCommon(E, "array_expr", label);

    printInitializerField(E->getInitializer(), "initializer");

    printList("elements", E->getElements(), [&](auto elt) {
      printRec(elt);
    });

    printFoot();
  }
  void visitDictionaryExpr(DictionaryExpr *E, PrintLabel label) {
    printCommon(E, "dictionary_expr", label);

    printInitializerField(E->getInitializer(), "initializer");

    printList("elements", E->getElements(), [&](auto elt) {
      printRec(elt);
    });

    printFoot();
  }
  void visitSubscriptExpr(SubscriptExpr *E, PrintLabel label) {
    printCommon(E, "subscript_expr", label);
    printThrowDest(E->throws(), /*wantNothrow=*/false);

    if (E->getAccessSemantics() != AccessSemantics::Ordinary)
      printFlag(getDumpString(E->getAccessSemantics()), AccessLevelColor);
    printFlag(E->isSuper(), "super");
    if (E->hasDecl()) {
      printDeclRefField(E->getDecl(), "decl");
    }

    printRec(E->getBase(), PrintLabel::suppressable("base"));
    printRec(E->getArgs(), PrintLabel::suppressable("args"));

    printFoot();
  }
  void visitKeyPathApplicationExpr(KeyPathApplicationExpr *E,
                                   PrintLabel label) {
    printCommon(E, "keypath_application_expr", label);
    printRec(E->getBase(), PrintLabel::suppressable("base"));
    printRec(E->getKeyPath(), PrintLabel::suppressable("key_path"));
    printFoot();
  }
  void visitDynamicSubscriptExpr(DynamicSubscriptExpr *E, PrintLabel label) {
    printCommon(E, "dynamic_subscript_expr", label);
    printThrowDest(E->throws(), /*wantNothrow=*/false);

    printDeclRefField(E->getMember(), "decl");

    printRec(E->getBase(), PrintLabel::suppressable("base"));
    printRec(E->getArgs(), PrintLabel::suppressable("args"));

    printFoot();
  }
  void visitUnresolvedDotExpr(UnresolvedDotExpr *E, PrintLabel label) {
    printCommon(E, "unresolved_dot_expr", label);

    printFieldQuoted(E->getName(), "field");
    printFieldRaw([&](auto &os) { E->getFunctionRefInfo().dump(os); },
                  "function_ref", ExprModifierColor);

    if (E->getBase()) {
      printRec(E->getBase(), PrintLabel::suppressable("base"));
    }

    printFoot();
  }
  void visitTupleElementExpr(TupleElementExpr *E, PrintLabel label) {
    printCommon(E, "tuple_element_expr", label);

    printField(E->getFieldNumber(), "field #");

    printRec(E->getBase(), PrintLabel::suppressable("base"));

    printFoot();
  }
  void visitDestructureTupleExpr(DestructureTupleExpr *E, PrintLabel label) {
    printCommon(E, "destructure_tuple_expr", label);

    printRecRange(E->getDestructuredElements(), "destructured");
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printRec(E->getResultExpr(), PrintLabel::suppressable("result_expr"));

    printFoot();
  }
  void visitUnresolvedTypeConversionExpr(UnresolvedTypeConversionExpr *E,
                                         PrintLabel label) {
    printCommon(E, "unresolvedtype_conversion_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitFunctionConversionExpr(FunctionConversionExpr *E,
                                   PrintLabel label) {
    printCommon(E, "function_conversion_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitCovariantFunctionConversionExpr(CovariantFunctionConversionExpr *E,
                                            PrintLabel label) {
    printCommon(E, "covariant_function_conversion_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitCovariantReturnConversionExpr(CovariantReturnConversionExpr *E,
                                          PrintLabel label) {
    printCommon(E, "covariant_return_conversion_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitUnderlyingToOpaqueExpr(UnderlyingToOpaqueExpr *E, PrintLabel label) {
    printCommon(E, "underlying_to_opaque_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitErasureExpr(ErasureExpr *E, PrintLabel label) {
    printCommon(E, "erasure_expr", label);
    printList("conformances", E->getConformances(), [&](auto conf) {
      printRec(conf);
    });
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitAnyHashableErasureExpr(AnyHashableErasureExpr *E,
                                   PrintLabel label) {
    printCommon(E, "any_hashable_erasure_expr", label);
    printRec(E->getConformance(), PrintLabel::suppressable("conformance"));
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitConditionalBridgeFromObjCExpr(ConditionalBridgeFromObjCExpr *E,
                                          PrintLabel label) {
    printCommon(E, "conditional_bridge_from_objc_expr", label);

    printDeclRefField(E->getConversion(), "conversion");

    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));

    printFoot();
  }
  void visitBridgeFromObjCExpr(BridgeFromObjCExpr *E, PrintLabel label) {
    printCommon(E, "bridge_from_objc_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitBridgeToObjCExpr(BridgeToObjCExpr *E, PrintLabel label) {
    printCommon(E, "bridge_to_objc_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitLoadExpr(LoadExpr *E, PrintLabel label) {
    printCommon(E, "load_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitABISafeConversionExpr(ABISafeConversionExpr *E, PrintLabel label) {
    printCommon(E, "abi_safe_conversion_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitMetatypeConversionExpr(MetatypeConversionExpr *E, PrintLabel label) {
    printCommon(E, "metatype_conversion_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitCollectionUpcastConversionExpr(CollectionUpcastConversionExpr *E,
                                           PrintLabel label) {
    printCommon(E, "collection_upcast_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    if (auto keyConversion = E->getKeyConversion()) {
      printRec(keyConversion.Conversion, "key_conversion");
    }
    if (auto valueConversion = E->getValueConversion()) {
      printRec(valueConversion.Conversion, "value_conversion");
    }
    printFoot();
  }
  void visitDerivedToBaseExpr(DerivedToBaseExpr *E, PrintLabel label) {
    printCommon(E, "derived_to_base_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitArchetypeToSuperExpr(ArchetypeToSuperExpr *E, PrintLabel label) {
    printCommon(E, "archetype_to_super_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitInjectIntoOptionalExpr(InjectIntoOptionalExpr *E, PrintLabel label) {
    printCommon(E, "inject_into_optional", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitClassMetatypeToObjectExpr(ClassMetatypeToObjectExpr *E,
                                      PrintLabel label) {
    printCommon(E, "class_metatype_to_object", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitExistentialMetatypeToObjectExpr(ExistentialMetatypeToObjectExpr *E,
                                            PrintLabel label) {
    printCommon(E, "existential_metatype_to_object", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitProtocolMetatypeToObjectExpr(ProtocolMetatypeToObjectExpr *E,
                                         PrintLabel label) {
    printCommon(E, "protocol_metatype_to_object", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitInOutToPointerExpr(InOutToPointerExpr *E, PrintLabel label) {
    printCommon(E, "inout_to_pointer", label);
    printFlag(E->isNonAccessing(), "nonaccessing");
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitArrayToPointerExpr(ArrayToPointerExpr *E, PrintLabel label) {
    printCommon(E, "array_to_pointer", label);
    printFlag(E->isNonAccessing(), "nonaccessing");
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitStringToPointerExpr(StringToPointerExpr *E, PrintLabel label) {
    printCommon(E, "string_to_pointer", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitPointerToPointerExpr(PointerToPointerExpr *E, PrintLabel label) {
    printCommon(E, "pointer_to_pointer", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitForeignObjectConversionExpr(ForeignObjectConversionExpr *E,
                                        PrintLabel label) {
    printCommon(E, "foreign_object_conversion", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitUnevaluatedInstanceExpr(UnevaluatedInstanceExpr *E,
                                    PrintLabel label) {
    printCommon(E, "unevaluated_instance", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitUnreachableExpr(UnreachableExpr *E, PrintLabel label) {
    printCommon(E, "unreachable", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitDifferentiableFunctionExpr(DifferentiableFunctionExpr *E,
                                       PrintLabel label) {
    printCommon(E, "differentiable_function", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitLinearFunctionExpr(LinearFunctionExpr *E, PrintLabel label) {
    printCommon(E, "linear_function", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitDifferentiableFunctionExtractOriginalExpr(
      DifferentiableFunctionExtractOriginalExpr *E, PrintLabel label) {
    printCommon(E, "differentiable_function_extract_original", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitLinearFunctionExtractOriginalExpr(
      LinearFunctionExtractOriginalExpr *E, PrintLabel label) {
    printCommon(E, "linear_function_extract_original", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitLinearToDifferentiableFunctionExpr(
      LinearToDifferentiableFunctionExpr *E, PrintLabel label) {
    printCommon(E, "linear_to_differentiable_function", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }

  void visitActorIsolationErasureExpr(ActorIsolationErasureExpr *E,
                                      PrintLabel label) {
    printCommon(E, "actor_isolation_erasure_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }

  void visitExtractFunctionIsolationExpr(ExtractFunctionIsolationExpr *E,
                                         PrintLabel label) {
    printCommon(E, "extract_function_isolation", label);
    printRec(E->getFunctionExpr(), PrintLabel::suppressable("function_expr"));
    printFoot();
  }

  void visitInOutExpr(InOutExpr *E, PrintLabel label) {
    printCommon(E, "inout_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }

  void visitVarargExpansionExpr(VarargExpansionExpr *E, PrintLabel label) {
    printCommon(E, "vararg_expansion_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }

  void visitPackExpansionExpr(PackExpansionExpr *E, PrintLabel label) {
    printCommon(E, "pack_expansion_expr", label);
    printRec(E->getPatternExpr(), PrintLabel::suppressable("pattern_expr"));
    printFoot();
  }

  void visitPackElementExpr(PackElementExpr *E, PrintLabel label) {
    printCommon(E, "pack_element_expr", label);
    printRec(E->getPackRefExpr(), PrintLabel::suppressable("pack_ref_expr"));
    printFoot();
  }

  void visitMaterializePackExpr(MaterializePackExpr *E, PrintLabel label) {
    printCommon(E, "materialize_pack_expr", label);
    printRec(E->getFromExpr(), PrintLabel::suppressable("from_expr"));
    printFoot();
  }

  void visitForceTryExpr(ForceTryExpr *E, PrintLabel label) {
    printCommon(E, "force_try_expr", label);

    PrintOptions PO;
    PO.PrintTypesForDebugging = true;
    printFieldQuoted(E->getThrownError().getString(PO), "thrown_error", TypeColor);

    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }

  void visitOptionalTryExpr(OptionalTryExpr *E, PrintLabel label) {
    printCommon(E, "optional_try_expr", label);

    PrintOptions PO;
    PO.PrintTypesForDebugging = true;
    printFieldQuoted(E->getThrownError().getString(PO), "thrown_error", TypeColor);

    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }

  void visitTryExpr(TryExpr *E, PrintLabel label) {
    printCommon(E, "try_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }

  void visitSequenceExpr(SequenceExpr *E, PrintLabel label) {
    printCommon(E, "sequence_expr", label);
    printList("elements", range(E->getNumElements()), [&](unsigned i) {
      printRec(E->getElement(i));
    });
    printFoot();
  }

  void visitCaptureListExpr(CaptureListExpr *E, PrintLabel label) {
    printCommon(E, "capture_list", label);
    printList("captures", E->getCaptureList(), [&](auto capture) {
      printRec(capture.PBD);
    });
    printRec(E->getClosureBody(), PrintLabel::suppressable("closure_body"));
    printFoot();
  }

  void printClosure(AbstractClosureExpr *E, char const *name,
                                  PrintLabel label) {
    printCommon(E, name, label);

    // If we aren't printing to standard error or the debugger output stream,
    // this client expects to see the computed discriminator. Compute it now.
    if (hasNonStandardOutput())
      (void)E->getDiscriminator();

    printField(E->getRawDiscriminator(), "discriminator", DiscriminatorColor);

    switch (auto isolation = E->getActorIsolation()) {
    case ActorIsolation::Unspecified:
    case ActorIsolation::NonisolatedUnsafe:
      break;

    case ActorIsolation::Nonisolated:
      printFlag(true, "nonisolated", CapturesColor);
      break;

    case ActorIsolation::Erased:
      printFlag(true, "dynamically_isolated", CapturesColor);
      break;

    case ActorIsolation::ActorInstance:
      printFieldQuoted(isolation.getActorInstance()->printRef(),
                       "actor_isolated", CapturesColor);
      break;

    case ActorIsolation::GlobalActor:
      printFieldQuoted(isolation.getGlobalActor().getString(),
                       "global_actor_isolated", CapturesColor);
      break;
    }

    if (auto captureInfo = E->getCachedCaptureInfo()) {
      if (!captureInfo->isTrivial()) {
        printFieldRaw([&](raw_ostream &OS) {
          captureInfo->print(OS);
        }, PrintLabel::suppressable("captures"), CapturesColor);
      }
    }
    // Printing a function type doesn't indicate whether it's escaping because it doesn't 
    // matter in 99% of contexts. AbstractClosureExpr nodes are one of the only exceptions.
    if (auto Ty = GetTypeOfExpr(E)) {
      if (auto fType = Ty->getAs<AnyFunctionType>()) {
        printFlag(!fType->getExtInfo().isNoEscape(), "escaping",
                  ClosureModifierColor);
        printFlag(fType->getExtInfo().isSendable(), "sendable",
                  ClosureModifierColor);
      }
    }
  }

  void visitClosureExpr(ClosureExpr *E, PrintLabel label) {
    printClosure(E, "closure_expr", label);
    printFlag(E->hasSingleExpressionBody(), "single_expression",
              ClosureModifierColor);
    printFlag(E->allowsImplicitSelfCapture(), "implicit_self",
              ClosureModifierColor);
    printFlag(E->inheritsActorContext(), "inherits_actor_context",
              ClosureModifierColor);

    if (E->getParameters()) {
      printRec(E->getParameters(), &E->getASTContext(),
               PrintLabel::suppressable("parameters"));
    }
    printRec(E->getBody(), &E->getASTContext(),
             PrintLabel::suppressable("body"));

    printFoot();
  }
  void visitAutoClosureExpr(AutoClosureExpr *E, PrintLabel label) {
    printClosure(E, "autoclosure_expr", label);

    if (E->getParameters()) {
      printRec(E->getParameters(), &E->getASTContext(),
               PrintLabel::suppressable("parameters"));
    }

    printRec(E->getSingleExpressionBody(),
             PrintLabel::suppressable("single_expression_body"));
    printFoot();
  }

  void visitDynamicTypeExpr(DynamicTypeExpr *E, PrintLabel label) {
    printCommon(E, "metatype_expr", label);
    printRec(E->getBase(), PrintLabel::suppressable("base"));
    printFoot();
  }

  void visitOpaqueValueExpr(OpaqueValueExpr *E, PrintLabel label) {
    printCommon(E, "opaque_value_expr", label);
    printNameRaw([&](raw_ostream &OS) { OS << (void*)E; });
    printFoot();
  }

  void visitPropertyWrapperValuePlaceholderExpr(
      PropertyWrapperValuePlaceholderExpr *E, PrintLabel label) {
    printCommon(E, "property_wrapper_value_placeholder_expr", label);
    printRec(E->getOpaqueValuePlaceholder(),
             PrintLabel::suppressable("opaque_value_placeholder"));
    if (auto *value = E->getOriginalWrappedValue()) {
      printRec(value, PrintLabel::suppressable("original_wrapped_value"));
    }
    printFoot();
  }

  void visitAppliedPropertyWrapperExpr(AppliedPropertyWrapperExpr *E,
                                       PrintLabel label) {
    printCommon(E, "applied_property_wrapper_expr", label);
    printRec(E->getValue(), PrintLabel::suppressable("value"));
    printFoot();
  }

  void visitDefaultArgumentExpr(DefaultArgumentExpr *E, PrintLabel label) {
    printCommon(E, "default_argument_expr", label);
    printDeclRefField(E->getDefaultArgsOwner(), "default_args_owner");
    printField(E->getParamIndex(), "param");
    printFoot();
  }

  void printApplyExpr(ApplyExpr *E, const char *NodeName, PrintLabel label) {
    printCommon(E, NodeName, label);
    if (E->isThrowsSet()) {
      printThrowDest(E->throws(), /*wantNothrow=*/true);
    }
    printFieldQuotedRaw([&](raw_ostream &OS) {
      auto isolationCrossing = E->getIsolationCrossing();
      if (isolationCrossing.has_value()) {
        OS << "{caller='";
        simple_display(OS, isolationCrossing.value().getCallerIsolation());
        OS << "', callee='";
        simple_display(OS, isolationCrossing.value().getCalleeIsolation());
        OS << "'}";
      } else {
        OS << "none";
      }
    }, "isolation_crossing", ExprModifierColor);

    printRec(E->getFn(), PrintLabel::suppressable("fn"));
    printRec(E->getArgs(), PrintLabel::suppressable("args"));

    printFoot();
  }

  void visitCallExpr(CallExpr *E, PrintLabel label) {
    printApplyExpr(E, "call_expr", label);
  }
  void visitPrefixUnaryExpr(PrefixUnaryExpr *E, PrintLabel label) {
    printApplyExpr(E, "prefix_unary_expr", label);
  }
  void visitPostfixUnaryExpr(PostfixUnaryExpr *E, PrintLabel label) {
    printApplyExpr(E, "postfix_unary_expr", label);
  }
  void visitBinaryExpr(BinaryExpr *E, PrintLabel label) {
    printApplyExpr(E, "binary_expr", label);
  }
  void visitDotSyntaxCallExpr(DotSyntaxCallExpr *E, PrintLabel label) {
    printApplyExpr(E, "dot_syntax_call_expr", label);
  }
  void visitConstructorRefCallExpr(ConstructorRefCallExpr *E, PrintLabel label) {
    printApplyExpr(E, "constructor_ref_call_expr", label);
  }
  void visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *E,
                                     PrintLabel label) {
    printCommon(E, "dot_syntax_base_ignored", label);
    printRec(E->getLHS(), PrintLabel::suppressable("lhs"));
    printRec(E->getRHS(), PrintLabel::suppressable("rhs"));
    printFoot();
  }

  void printExplicitCastExpr(ExplicitCastExpr *E, const char *name,
                             PrintLabel label) {
    printCommon(E, name, label);

    if (auto checkedCast = dyn_cast<CheckedCastExpr>(E))
      printFlag(getDumpString(checkedCast->getCastKind()));
    printFieldQuotedRaw([&](raw_ostream &OS) {
      if (GetTypeOfTypeRepr)
        GetTypeOfTypeRepr(E->getCastTypeRepr()).print(OS);
      else
        E->getCastType().print(OS);
    }, "written_type", TypeReprColor);

    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitForcedCheckedCastExpr(ForcedCheckedCastExpr *E, PrintLabel label) {
    printExplicitCastExpr(E, "forced_checked_cast_expr", label);
  }
  void visitConditionalCheckedCastExpr(ConditionalCheckedCastExpr *E,
                                       PrintLabel label) {
    printExplicitCastExpr(E, "conditional_checked_cast_expr", label);
  }
  void visitIsExpr(IsExpr *E, PrintLabel label) {
    printExplicitCastExpr(E, "is_subtype_expr", label);
  }
  void visitCoerceExpr(CoerceExpr *E, PrintLabel label) {
    printExplicitCastExpr(E, "coerce_expr", label);
  }
  void visitArrowExpr(ArrowExpr *E, PrintLabel label) {
    printCommon(E, "arrow", label);

    printFlag(E->getAsyncLoc().isValid(), "async");
    printFlag(E->getThrowsLoc().isValid(), "throws");

    printRec(E->getArgsExpr(), PrintLabel::suppressable("args_expr"));
    printRec(E->getResultExpr(), PrintLabel::suppressable("result_expr"));

    printFoot();
  }
  void visitRebindSelfInConstructorExpr(RebindSelfInConstructorExpr *E,
                                        PrintLabel label) {
    printCommon(E, "rebind_self_in_constructor_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitTernaryExpr(TernaryExpr *E, PrintLabel label) {
    printCommon(E, "ternary_expr", label);
    printRec(E->getCondExpr(), PrintLabel::suppressable("cond_expr"));
    printRec(E->getThenExpr(), PrintLabel::suppressable("then_expr"));
    printRec(E->getElseExpr(), PrintLabel::suppressable("else_expr"));
    printFoot();
  }
  void visitAssignExpr(AssignExpr *E, PrintLabel label) {
    printCommon(E, "assign_expr", label);
    printRec(E->getDest(), PrintLabel::suppressable("dest"));
    printRec(E->getSrc(), PrintLabel::suppressable("src"));
    printFoot();
  }
  void visitEnumIsCaseExpr(EnumIsCaseExpr *E, PrintLabel label) {
    printCommon(E, "enum_is_case_expr", label);
    printName(E->getEnumElement()->getBaseIdentifier());
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitUnresolvedPatternExpr(UnresolvedPatternExpr *E, PrintLabel label) {
    printCommon(E, "unresolved_pattern_expr", label);
    printRec(E->getSubPattern(), PrintLabel::suppressable("sub_pattern"));
    printFoot();
  }
  void visitBindOptionalExpr(BindOptionalExpr *E, PrintLabel label) {
    printCommon(E, "bind_optional_expr", label);
    printField(E->getDepth(), "depth");
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitOptionalEvaluationExpr(OptionalEvaluationExpr *E, PrintLabel label) {
    printCommon(E, "optional_evaluation_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitForceValueExpr(ForceValueExpr *E, PrintLabel label) {
    printCommon(E, "force_value_expr", label);

    printFlag(E->isForceOfImplicitlyUnwrappedOptional(), "implicit_iuo_unwrap",
              ExprModifierColor);

    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));

    printFoot();
  }
  void visitOpenExistentialExpr(OpenExistentialExpr *E, PrintLabel label) {
    printCommon(E, "open_existential_expr", label);
    printRec(E->getOpaqueValue(), PrintLabel::suppressable("opaque_value"));
    printRec(E->getExistentialValue(),
             PrintLabel::suppressable("existential_value"));
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitMakeTemporarilyEscapableExpr(MakeTemporarilyEscapableExpr *E,
                                         PrintLabel label) {
    printCommon(E, "make_temporarily_escapable_expr", label);
    printRec(E->getOpaqueValue(), PrintLabel::suppressable("opaque_value"));
    printRec(E->getNonescapingClosureValue(),
             PrintLabel::suppressable("nonescaping_closure_value"));
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitEditorPlaceholderExpr(EditorPlaceholderExpr *E, PrintLabel label) {
    printCommon(E, "editor_placeholder_expr", label);

    auto *TyR = E->getPlaceholderTypeRepr();
    auto *ExpTyR = E->getTypeForExpansion();
    if (TyR)
      printRec(TyR, PrintLabel::suppressable("type_repr"));
    if (ExpTyR && ExpTyR != TyR) {
      printRec(ExpTyR, PrintLabel::suppressable("type_for_expansion"));
    }
    printSemanticExpr(E->getSemanticExpr());
    printFoot();
  }
  void visitLazyInitializerExpr(LazyInitializerExpr *E, PrintLabel label) {
    printCommon(E, "lazy_initializer_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }
  void visitObjCSelectorExpr(ObjCSelectorExpr *E, PrintLabel label) {
    printCommon(E, "objc_selector_expr", label);

    printField(E->getSelectorKind(), "kind");
    printDeclRefField(E->getMethod(), "decl");

    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));

    printFoot();
  }

  void visitKeyPathExpr(KeyPathExpr *E, PrintLabel label) {
    printCommon(E, "keypath_expr", label);

    printFlag(E->isObjC(), "objc");

    auto printComponents = [&]{
      printList("components", indices(E->getComponents()), [&](unsigned i) {
        auto &component = E->getComponents()[i];
        printRecArbitrary([&](PrintLabel label) {
          switch (component.getKind()) {
          case KeyPathExpr::Component::Kind::Invalid:
            printHead("invalid", ASTNodeColor);
            break;

          case KeyPathExpr::Component::Kind::OptionalChain:
            printHead("optional_chain", ASTNodeColor);
            break;

          case KeyPathExpr::Component::Kind::OptionalForce:
            printHead("optional_force", ASTNodeColor);
            break;

          case KeyPathExpr::Component::Kind::OptionalWrap:
            printHead("optional_wrap", ASTNodeColor);
            break;

          case KeyPathExpr::Component::Kind::Property:
            printHead("property", ASTNodeColor);
            printDeclRefField(component.getDeclRef(), "decl");
            break;

          case KeyPathExpr::Component::Kind::Subscript:
            printHead("subscript", ASTNodeColor);
            printDeclRefField(component.getDeclRef(), "decl");
            break;

          case KeyPathExpr::Component::Kind::UnresolvedProperty:
            printHead("unresolved_property", ASTNodeColor);
            printFieldQuoted(component.getUnresolvedDeclName(), "decl_name",
                            IdentifierColor);
            break;

          case KeyPathExpr::Component::Kind::UnresolvedSubscript:
            printHead("unresolved_subscript", ASTNodeColor);
            break;
          case KeyPathExpr::Component::Kind::Identity:
            printHead("identity", ASTNodeColor);
            break;

          case KeyPathExpr::Component::Kind::TupleElement:
            printHead("tuple_element", ASTNodeColor);
            printField(component.getTupleIndex(), "index", DiscriminatorColor);
            break;
          case KeyPathExpr::Component::Kind::DictionaryKey:
            printHead("dict_key", ASTNodeColor);
            printFieldQuoted(component.getUnresolvedDeclName(), "key",
                            IdentifierColor);
            break;
          case KeyPathExpr::Component::Kind::CodeCompletion:
            printHead("completion", ASTNodeColor);
            break;
          }
          printFieldQuoted(GetTypeOfKeyPathComponent(E, i), "type");
          if (auto *args = component.getSubscriptArgs()) {
            printRec(args, PrintLabel::suppressable("args"));
          }
          printFoot();
        });
      });
    };

    if (Writer.isParsable()) {
      printComponents();
    } else {
      // `printList` in default mode simply indents, so we need to preserve the
      // additional level of `(components ...)` for that mode.
      printRecArbitrary([&](PrintLabel label) {
        printHead("components", ExprColor, label);
        printComponents();
        printFoot();
      }, PrintLabel::suppressable("components"));
    }

    if (auto stringLiteral = E->getObjCStringLiteralExpr()) {
      printRec(stringLiteral, "objc_string_literal");
    }
    if (!E->isObjC()) {
      if (auto root = E->getParsedRoot()) {
        printRec(root, "parsed_root");
      }
      if (auto path = E->getParsedPath()) {
        printRec(path, "parsed_path");
      }
    }
    printFoot();
  }

  void visitCurrentContextIsolationExpr(
      CurrentContextIsolationExpr *E, PrintLabel label) {
    printCommon(E, "current_context_isolation_expr", label);
    if (auto actor = E->getActor())
      printRec(actor, PrintLabel::suppressable("actor"));

    printFoot();
  }

  void visitKeyPathDotExpr(KeyPathDotExpr *E, PrintLabel label) {
    printCommon(E, "key_path_dot_expr", label);
    printFoot();
  }

  void visitSingleValueStmtExpr(SingleValueStmtExpr *E, PrintLabel label) {
    printCommon(E, "single_value_stmt_expr", label);
    printRec(E->getStmt(), &E->getDeclContext()->getASTContext(),
             PrintLabel::suppressable("stmt"));
    printFoot();
  }

  void visitOneWayExpr(OneWayExpr *E, PrintLabel label) {
    printCommon(E, "one_way_expr", label);
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printFoot();
  }

  void visitTapExpr(TapExpr *E, PrintLabel label) {
    printCommon(E, "tap_expr", label);
    printDeclRefField(E->getVar(), "var");
    printRec(E->getSubExpr(), PrintLabel::suppressable("sub_expr"));
    printRec(E->getBody(), &E->getVar()->getDeclContext()->getASTContext(),
             PrintLabel::suppressable("body"));
    printFoot();
  }

  void visitTypeJoinExpr(TypeJoinExpr *E, PrintLabel label) {
    printCommon(E, "type_join_expr", label);

    if (auto *var = E->getVar()) {
      printRec(var, "var");
    }

    if (auto *SVE = E->getSingleValueStmtExpr()) {
      printRec(SVE, "single_value_stmt_expr");
    }

    printList("elements", E->getElements(), [&](auto *member) {
      printRec(member);
    });

    printFoot();
  }

  void visitMacroExpansionExpr(MacroExpansionExpr *E, PrintLabel label) {
    printCommon(E, "macro_expansion_expr", label);

    printFieldQuoted(E->getMacroName(), "name", IdentifierColor);
    printField(E->getRawDiscriminator(), "discriminator", DiscriminatorColor);

    if (E->getArgs()) {
      printRec(E->getArgs(), PrintLabel::suppressable("args"));
    }
    if (auto rewritten = E->getRewritten()) {
      printRec(rewritten, "rewritten");
    }

    printFoot();
  }

  void visitTypeValueExpr(TypeValueExpr *E, PrintLabel label) {
    printCommon(E, "type_value_expr", label);

    PrintOptions PO;
    PO.PrintTypesForDebugging = true;
    printFieldQuoted(Type(E->getParamType()).getString(PO), "param_type",
                     TypeColor);

    printFoot();
  }
};

} // end anonymous namespace

void Expr::dump() const {
  dump(llvm::errs());
  llvm::errs() << "\n";
}

void Expr::dump(raw_ostream &OS, llvm::function_ref<Type(Expr *)> getTypeOfExpr,
                llvm::function_ref<Type(TypeRepr *)> getTypeOfTypeRepr,
                llvm::function_ref<Type(KeyPathExpr *E, unsigned index)>
                    getTypeOfKeyPathComponent,
                unsigned Indent) const {
  DefaultWriter writer(OS, Indent);
  PrintExpr(writer, /*parseIfNeeded*/ false, getTypeOfExpr,
            getTypeOfTypeRepr, getTypeOfKeyPathComponent)
      .visit(const_cast<Expr *>(this), "");
}

void Expr::dump(raw_ostream &OS, unsigned Indent) const {
  dump(OS, defaultGetTypeOfExpr, /*getTypeOfTypeRepr*/ nullptr,
       defaultGetTypeOfKeyPathComponent, Indent);
}

void Expr::print(ASTPrinter &Printer, const PrintOptions &Opts) const {
  // FIXME: Fully use the ASTPrinter.
  llvm::SmallString<128> Str;
  llvm::raw_svector_ostream OS(Str);
  dump(OS);
  Printer << OS.str();
}

void ArgumentList::dump() const {
  dump(llvm::errs(), 0);
  llvm::errs() << '\n';
}

void ArgumentList::dump(raw_ostream &OS, unsigned Indent) const {
  DefaultWriter writer(OS, Indent);
  PrintBase(writer).visitArgumentList(this);
}

//===----------------------------------------------------------------------===//
// Printing for TypeRepr and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
class PrintTypeRepr : public TypeReprVisitor<PrintTypeRepr, void, PrintLabel>,
                      public PrintBase {
public:
  using PrintBase::PrintBase;

  void printCommon(const char *Name, PrintLabel Label) {
    printHead(Name, TypeReprColor, Label);
  }

  void visitErrorTypeRepr(ErrorTypeRepr *T, PrintLabel label) {
    printCommon("type_error", label);
  }

  void visitAttributedTypeRepr(AttributedTypeRepr *T, PrintLabel label) {
    printCommon("type_attributed", label);
    printFieldQuotedRaw([&](raw_ostream &OS) { T->printAttrs(OS); }, "attrs");
    printRec(T->getTypeRepr(), PrintLabel::suppressable("type_repr"));
  }

  void visitDeclRefTypeRepr(DeclRefTypeRepr *T, PrintLabel label) {
    printCommon(isa<UnqualifiedIdentTypeRepr>(T) ? "type_unqualified_ident"
                                                 : "type_qualified_ident",
                label);

    printFieldQuoted(T->getNameRef(), "id", IdentifierColor);
    if (T->isBound())
      printFieldQuoted(T->getBoundDecl()->printRef(), "bind");
    else
      printFlag("unbound");

    if (auto *qualIdentTR = dyn_cast<QualifiedIdentTypeRepr>(T)) {
      printRec(qualIdentTR->getBase(), PrintLabel::suppressable("base"));
    }

    printList("genericArgs", T->getGenericArgs(), [&](auto *genArg) {
      printRec(genArg);
    });

    printFoot();
  }

  void visitFunctionTypeRepr(FunctionTypeRepr *T, PrintLabel label) {
    printCommon("type_function", label);

    printFlag(T->isAsync(), "async");
    printFlag(T->isThrowing(), "throws");

    printRec(T->getArgsTypeRepr(), PrintLabel::suppressable("args_type_repr"));
    printRec(T->getResultTypeRepr(),
             PrintLabel::suppressable("result_type_repr"));

    printFoot();
  }

  void visitInverseTypeRepr(InverseTypeRepr *T, PrintLabel label) {
    printCommon("inverse", label);
    printRec(T->getConstraint(), PrintLabel::suppressable("constraint"));
    printFoot();
  }

  void visitArrayTypeRepr(ArrayTypeRepr *T, PrintLabel label) {
    printCommon("type_array", label);
    printRec(T->getBase(), PrintLabel::suppressable("base"));
    printFoot();
  }

  void visitDictionaryTypeRepr(DictionaryTypeRepr *T, PrintLabel label) {
    printCommon("type_dictionary", label);
    printRec(T->getKey(), PrintLabel::suppressable("key"));
    printRec(T->getValue(), PrintLabel::suppressable("value"));
    printFoot();
  }

  void visitVarargTypeRepr(VarargTypeRepr *T, PrintLabel label) {
    printCommon("vararg", label);
    printRec(T->getElementType(), PrintLabel::suppressable("element_type"));
    printFoot();
  }

  void visitPackTypeRepr(PackTypeRepr *T, PrintLabel label) {
    printCommon("pack", label);
    printList("elements", T->getElements(), [&](auto elt) {
      printRec(elt);
    });
    printFoot();
  }

  void visitPackExpansionTypeRepr(PackExpansionTypeRepr *T, PrintLabel label) {
    printCommon("pack_expansion", label);
    printRec(T->getPatternType(), PrintLabel::suppressable("pattern_type"));
    printFoot();
  }

  void visitPackElementTypeRepr(PackElementTypeRepr *T, PrintLabel label) {
    printCommon("pack_element", label);
    printRec(T->getPackType(), PrintLabel::suppressable("pack_type"));
    printFoot();
  }

  void visitTupleTypeRepr(TupleTypeRepr *T, PrintLabel label) {
    printCommon("type_tuple", label);

    if (T->hasElementNames()) {
      printFieldQuotedRaw([&](raw_ostream &OS) {
        llvm::interleave(T->getElements(), OS,
                         [&](const TupleTypeReprElement &Elt) {
          auto name = Elt.Name;
          if (Elt.UnderscoreLoc.isValid())
            OS << (name.empty() ? "_" : "_ " + name.str());
          else
            OS << (name.empty() ? "''" : name.str());
        }, ",");
      }, "names");
    }

    printList("elements", T->getElements(), [&](auto elem) {
      printRec(elem.Type);
    });

    printFoot();
  }

  void visitCompositionTypeRepr(CompositionTypeRepr *T, PrintLabel label) {
    printCommon("type_composite", label);
    printList("types", T->getTypes(), [&](auto elem) {
      printRec(elem);
    });
    printFoot();
  }

  void visitMetatypeTypeRepr(MetatypeTypeRepr *T, PrintLabel label) {
    printCommon("type_metatype", label);
    printRec(T->getBase(), PrintLabel::suppressable("base"));
    printFoot();
  }

  void visitProtocolTypeRepr(ProtocolTypeRepr *T, PrintLabel label) {
    printCommon("type_protocol", label);
    printRec(T->getBase(), PrintLabel::suppressable("base"));
    printFoot();
  }

  void visitOwnershipTypeRepr(OwnershipTypeRepr *T, PrintLabel label) {
    printCommon("type_ownership", label);
    printFlag(getDumpString(T->getSpecifier()));
    printRec(T->getBase(), PrintLabel::suppressable("base"));
    printFoot();
  }
  
  void visitIsolatedTypeRepr(IsolatedTypeRepr *T, PrintLabel label) {
    printCommon("isolated", label);
    printRec(T->getBase(), PrintLabel::suppressable("base"));
    printFoot();
  }

  void visitSendingTypeRepr(SendingTypeRepr *T, PrintLabel label) {
    printCommon("sending", label);
    printRec(T->getBase(), PrintLabel::suppressable("base"));
    printFoot();
  }

  void visitCompileTimeConstTypeRepr(CompileTimeConstTypeRepr *T,
                                     PrintLabel label) {
    printCommon("_const", label);
    printRec(T->getBase(), PrintLabel::suppressable("base"));
    printFoot();
  }

  void visitOptionalTypeRepr(OptionalTypeRepr *T, PrintLabel label) {
    printCommon("type_optional", label);
    printRec(T->getBase(), PrintLabel::suppressable("base"));
    printFoot();
  }

  void visitImplicitlyUnwrappedOptionalTypeRepr(
      ImplicitlyUnwrappedOptionalTypeRepr *T, PrintLabel label) {
    printCommon("type_implicitly_unwrapped_optional", label);
    printRec(T->getBase(), PrintLabel::suppressable("base"));
    printFoot();
  }

  void visitOpaqueReturnTypeRepr(OpaqueReturnTypeRepr *T, PrintLabel label) {
    printCommon("type_opaque_return", label);
    printRec(T->getConstraint(), PrintLabel::suppressable("constraint"));
    printFoot();
  }

  void visitNamedOpaqueReturnTypeRepr(NamedOpaqueReturnTypeRepr *T,
                                      PrintLabel label) {
    printCommon("type_named_opaque_return", label);
    printRec(T->getBase(), PrintLabel::suppressable("base"));
    printFoot();
  }

  void visitExistentialTypeRepr(ExistentialTypeRepr *T, PrintLabel label) {
    printCommon("type_existential", label);
    printRec(T->getConstraint(), PrintLabel::suppressable("constraint"));
    printFoot();
  }

  void visitPlaceholderTypeRepr(PlaceholderTypeRepr *T, PrintLabel label) {
    printCommon("type_placeholder", label);
    printFoot();
  }

  void visitFixedTypeRepr(FixedTypeRepr *T, PrintLabel label) {
    printCommon("type_fixed", label);

    auto Ty = T->getType();
    if (Ty) {
      printSourceLoc(T->getLoc(), &Ty->getASTContext());
    }

    printRec(Ty, "type");

    printFoot();
  }

  void visitSelfTypeRepr(SelfTypeRepr *T, PrintLabel label) {
    printCommon("type_self", label);

    auto Ty = T->getType();
    if (Ty) {
      printSourceLoc(T->getLoc(), &Ty->getASTContext());
    }

    printRec(Ty, "type");

    printFoot();
  }

  void visitSILBoxTypeRepr(SILBoxTypeRepr *T, PrintLabel label) {
    printCommon("sil_box", label);

    printList("fields", T->getFields(), [&](auto &Field) {
      printRecArbitrary([&](PrintLabel label) {
        printCommon("sil_box_field", label);
        printFlag(Field.isMutable(), "mutable");

        printRec(Field.getFieldType());
        printFoot();
      });
    });
    printList("generic_arguments", T->getGenericArguments(), [&](auto arg) {
      printRec(arg);
    });

    printFoot();
  }

  void visitLifetimeDependentTypeRepr(LifetimeDependentTypeRepr *T,
                                      PrintLabel label) {
    printCommon("type_lifetime_dependent_return", label);

    printFieldRaw(
        [&](raw_ostream &out) {
          out << " " << T->getLifetimeEntry()->getString() << " ";
        },
        "");
    printRec(T->getBase(), PrintLabel::suppressable("base"));
    printFoot();
  }

  void visitIntegerTypeRepr(IntegerTypeRepr *T, PrintLabel label) {
    printCommon("type_integer", label);

    if (T->getMinusLoc()) {
      printCommon("is_negative", label);
    }

    printFieldQuoted(T->getValue(), "value", IdentifierColor);
    printFoot();
  }
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Dumping for DeclAttributes
//===----------------------------------------------------------------------===//

namespace {
class PrintAttribute : public AttributeVisitor<PrintAttribute, void, PrintLabel>,
                       public PrintBase {
  const ASTContext *Ctx;

public:
  PrintAttribute(
      PrintWriterBase &writer, const ASTContext *ctx,
      bool parseIfNeeded = false,
      llvm::function_ref<Type(Expr *)> getTypeOfExpr = defaultGetTypeOfExpr,
      llvm::function_ref<Type(TypeRepr *)> getTypeOfTypeRepr = nullptr,
      llvm::function_ref<Type(KeyPathExpr *E, unsigned index)>
          getTypeOfKeyPathComponent = defaultGetTypeOfKeyPathComponent)
      : PrintBase(writer, parseIfNeeded, getTypeOfExpr, getTypeOfTypeRepr,
                  getTypeOfKeyPathComponent),
        Ctx(ctx) {}

  void printCommon(DeclAttribute *Attr, StringRef name, PrintLabel label) {
    printHead(name, DeclAttributeColor, label);
    printFlag(Attr->isImplicit(), "implicit");
    printFlag(Attr->isInvalid(), "invalid");
    printFlag(Attr->getAddedByAccessNote(), "added_by_access_note");
    printSourceRange(Attr->Range, Ctx);
  }

  /// Deleting this ensures that all attributes are covered by the
  /// visitor below.
  void visitDeclAttribute(DeclAttribute *A) = delete;

#define TRIVIAL_ATTR_PRINTER(Class, Name)                                      \
  void visit##Class##Attr(Class##Attr *Attr, PrintLabel label) {               \
    printCommon(Attr, #Name "_attr", label);                                   \
    printFoot();                                                               \
  }

  TRIVIAL_ATTR_PRINTER(Actor, actor)
  TRIVIAL_ATTR_PRINTER(AddressableSelf, _addressableSelf)
  TRIVIAL_ATTR_PRINTER(AlwaysEmitConformanceMetadata,
                       always_emit_conformance_metadata)
  TRIVIAL_ATTR_PRINTER(AlwaysEmitIntoClient, always_emit_into_client)
  TRIVIAL_ATTR_PRINTER(Async, async)
  TRIVIAL_ATTR_PRINTER(AtReasync, at_reasync)
  TRIVIAL_ATTR_PRINTER(AtRethrows, at_rethrows)
  TRIVIAL_ATTR_PRINTER(Borrowed, borrowed)
  TRIVIAL_ATTR_PRINTER(Borrowing, borrowing)
  TRIVIAL_ATTR_PRINTER(CompileTimeConst, compile_time_const)
  TRIVIAL_ATTR_PRINTER(CompilerInitialized, compiler_initialized)
  TRIVIAL_ATTR_PRINTER(Consuming, consuming)
  TRIVIAL_ATTR_PRINTER(Convenience, convenience)
  TRIVIAL_ATTR_PRINTER(DiscardableResult, discardable_result)
  TRIVIAL_ATTR_PRINTER(DisfavoredOverload, disfavored_overload)
  TRIVIAL_ATTR_PRINTER(DistributedActor, distributed_actor)
  TRIVIAL_ATTR_PRINTER(Dynamic, dynamic)
  TRIVIAL_ATTR_PRINTER(DynamicCallable, dynamic_callable)
  TRIVIAL_ATTR_PRINTER(DynamicMemberLookup, dynamic_member_lookup)
  TRIVIAL_ATTR_PRINTER(EagerMove, eager_move)
  TRIVIAL_ATTR_PRINTER(EmitAssemblyVisionRemarks, emit_assembly_vision_remarks)
  TRIVIAL_ATTR_PRINTER(Exported, exported)
  TRIVIAL_ATTR_PRINTER(ExtractConstantsFromMembers,
                       extract_constants_from_members)
  TRIVIAL_ATTR_PRINTER(Final, final)
  TRIVIAL_ATTR_PRINTER(FixedLayout, fixed_layout)
  TRIVIAL_ATTR_PRINTER(ForbidSerializingReference, forbid_serializing_reference)
  TRIVIAL_ATTR_PRINTER(Frozen, frozen)
  TRIVIAL_ATTR_PRINTER(GKInspectable, gk_inspectable)
  TRIVIAL_ATTR_PRINTER(GlobalActor, global_actor)
  TRIVIAL_ATTR_PRINTER(HasInitialValue, has_initial_value)
  TRIVIAL_ATTR_PRINTER(HasMissingDesignatedInitializers,
                       has_missing_designated_initializers)
  TRIVIAL_ATTR_PRINTER(HasStorage, has_storage)
  TRIVIAL_ATTR_PRINTER(IBAction, ib_action)
  TRIVIAL_ATTR_PRINTER(IBDesignable, ib_designable)
  TRIVIAL_ATTR_PRINTER(IBInspectable, ib_inspectable)
  TRIVIAL_ATTR_PRINTER(IBOutlet, ib_outlet)
  TRIVIAL_ATTR_PRINTER(IBSegueAction, ib_segue_action)
  TRIVIAL_ATTR_PRINTER(ImplementationOnly, implementation_only)
  TRIVIAL_ATTR_PRINTER(ImplicitSelfCapture, implicit_self_capture)
  TRIVIAL_ATTR_PRINTER(Indirect, indirect)
  TRIVIAL_ATTR_PRINTER(Infix, infix)
  TRIVIAL_ATTR_PRINTER(InheritActorContext, inherit_actor_context)
  TRIVIAL_ATTR_PRINTER(InheritsConvenienceInitializers,
                       inherits_convenience_initializers)
  TRIVIAL_ATTR_PRINTER(Inlinable, inlinable)
  TRIVIAL_ATTR_PRINTER(Isolated, isolated)
  TRIVIAL_ATTR_PRINTER(KnownToBeLocal, known_to_be_local)
  TRIVIAL_ATTR_PRINTER(LLDBDebuggerFunction, lldb_debugger_function)
  TRIVIAL_ATTR_PRINTER(Lazy, lazy)
  TRIVIAL_ATTR_PRINTER(LegacyConsuming, legacy_consuming)
  TRIVIAL_ATTR_PRINTER(LexicalLifetimes, lexical_lifetimes)
  TRIVIAL_ATTR_PRINTER(MainType, main_type)
  TRIVIAL_ATTR_PRINTER(Marker, marker)
  TRIVIAL_ATTR_PRINTER(MoveOnly, move_only)
  TRIVIAL_ATTR_PRINTER(Mutating, mutating)
  TRIVIAL_ATTR_PRINTER(NSApplicationMain, ns_application_main)
  TRIVIAL_ATTR_PRINTER(NSCopying, ns_copying)
  TRIVIAL_ATTR_PRINTER(NSManaged, ns_managed)
  TRIVIAL_ATTR_PRINTER(NoAllocation, no_allocation)
  TRIVIAL_ATTR_PRINTER(NoDerivative, no_derivative)
  TRIVIAL_ATTR_PRINTER(NoEagerMove, no_eager_move)
  TRIVIAL_ATTR_PRINTER(NoExistentials, no_existentials)
  TRIVIAL_ATTR_PRINTER(NoImplicitCopy, no_implicit_copy)
  TRIVIAL_ATTR_PRINTER(NoLocks, no_locks)
  TRIVIAL_ATTR_PRINTER(NoMetadata, no_metadata)
  TRIVIAL_ATTR_PRINTER(NoObjCBridging, no_objc_bridging)
  TRIVIAL_ATTR_PRINTER(NoRuntime, no_runtime)
  TRIVIAL_ATTR_PRINTER(NonEphemeral, non_ephemeral)
  TRIVIAL_ATTR_PRINTER(NonEscapable, non_escapable)
  TRIVIAL_ATTR_PRINTER(NonMutating, non_mutating)
  TRIVIAL_ATTR_PRINTER(NonObjC, non_objc)
  TRIVIAL_ATTR_PRINTER(NonOverride, non_override)
  TRIVIAL_ATTR_PRINTER(ObjCMembers, objc_members)
  TRIVIAL_ATTR_PRINTER(ObjCNonLazyRealization, objc_non_lazy_realization)
  TRIVIAL_ATTR_PRINTER(Optional, optional)
  TRIVIAL_ATTR_PRINTER(Override, override)
  TRIVIAL_ATTR_PRINTER(Postfix, postfix)
  TRIVIAL_ATTR_PRINTER(PreInverseGenerics, pre_inverse_generics)
  TRIVIAL_ATTR_PRINTER(Preconcurrency, preconcurrency)
  TRIVIAL_ATTR_PRINTER(Prefix, prefix)
  TRIVIAL_ATTR_PRINTER(PropertyWrapper, property_wrapper)
  TRIVIAL_ATTR_PRINTER(Reasync, reasync)
  TRIVIAL_ATTR_PRINTER(Required, required)
  TRIVIAL_ATTR_PRINTER(RequiresStoredPropertyInits,
                       requires_stored_property_inits)
  TRIVIAL_ATTR_PRINTER(ResultBuilder, result_builder)
  TRIVIAL_ATTR_PRINTER(Rethrows, rethrows)
  TRIVIAL_ATTR_PRINTER(SPIOnly, spi_only)
  TRIVIAL_ATTR_PRINTER(Sendable, sendable)
  TRIVIAL_ATTR_PRINTER(Sensitive, sensitive)
  TRIVIAL_ATTR_PRINTER(ShowInInterface, show_in_interface)
  TRIVIAL_ATTR_PRINTER(SpecializeExtension, specialize_extension)
  TRIVIAL_ATTR_PRINTER(StaticExclusiveOnly, static_exclusive_only)
  TRIVIAL_ATTR_PRINTER(StaticInitializeObjCMetadata,
                       static_initialize_objc_metadata)
  TRIVIAL_ATTR_PRINTER(Testable, testable)
  TRIVIAL_ATTR_PRINTER(Transparent, transparent)
  TRIVIAL_ATTR_PRINTER(UIApplicationMain, ui_application_main)
  TRIVIAL_ATTR_PRINTER(Unsafe, unsafe)
  TRIVIAL_ATTR_PRINTER(UnsafeInheritExecutor, unsafe_inherit_executor)
  TRIVIAL_ATTR_PRINTER(UnsafeNoObjCTaggedPointer, unsafe_no_objc_tagged_pointer)
  TRIVIAL_ATTR_PRINTER(UnsafeNonEscapableResult, unsafe_non_escapable_result)
  TRIVIAL_ATTR_PRINTER(UsableFromInline, usable_from_inline)
  TRIVIAL_ATTR_PRINTER(Used, used)
  TRIVIAL_ATTR_PRINTER(WarnUnqualifiedAccess, warn_unqualified_access)
  TRIVIAL_ATTR_PRINTER(WeakLinked, weak_linked)

#undef TRIVIAL_ATTR_PRINTER

  void visitAccessControlAttr(AccessControlAttr *Attr, PrintLabel label) {
    printCommon(Attr, "access_control_attr", label);
    printField(Attr->getAccess(), "access_level");
    printFoot();
  }
  void visitAlignmentAttr(AlignmentAttr *Attr, PrintLabel label) {
    printCommon(Attr, "alignment_attr", label);
    printField(Attr->getValue(), "value");
    printFoot();
  }
  void visitAllowFeatureSuppressionAttr(AllowFeatureSuppressionAttr *Attr,
                                        PrintLabel label) {
    printCommon(Attr, "allow_feature_suppression_attr", label);
    printFieldQuotedRaw(
        [&](auto &out) {
          llvm::interleave(Attr->getSuppressedFeatures(), out, ",");
        },
        "features");
    printFoot();
  }
  void visitAvailableAttr(AvailableAttr *Attr, PrintLabel label) {
    printCommon(Attr, "available_attr", label);
    printField(Attr->getPlatform(), "platform");
    if (!Attr->Message.empty())
      printFieldQuoted(Attr->Message, "message");
    if (!Attr->Rename.empty())
      printFieldQuoted(Attr->Rename, "rename");
    if (Attr->Introduced.has_value())
      printFieldRaw(
          [&](auto &out) { out << Attr->Introduced.value().getAsString(); },
          "introduced");
    if (Attr->Deprecated.has_value())
      printFieldRaw(
          [&](auto &out) { out << Attr->Deprecated.value().getAsString(); },
          "deprecated");
    if (Attr->Obsoleted.has_value())
      printFieldRaw(
          [&](auto &out) { out << Attr->Obsoleted.value().getAsString(); },
          "obsoleted");
    printFoot();
  }
  void visitBackDeployedAttr(BackDeployedAttr *Attr, PrintLabel label) {
    printCommon(Attr, "back_deployed_attr", label);
    printField(Attr->Platform, "platform");
    printFieldRaw([&](auto &out) { out << Attr->Version.getAsString(); },
                  "version");
    printFoot();
  }
  void visitCDeclAttr(CDeclAttr *Attr, PrintLabel label) {
    printCommon(Attr, "cdecl_attr", label);
    printFieldQuoted(Attr->Name, "name");
    printFoot();
  }
  void
  visitClangImporterSynthesizedTypeAttr(ClangImporterSynthesizedTypeAttr *Attr,
                                        PrintLabel label) {
    printCommon(Attr, "clang_importer_synthesized_type_attr", label);
    printField(Attr->getKind(), "kind");
    printField(Attr->originalTypeName, "original_type_name");
    printFoot();
  }
  void visitCustomAttr(CustomAttr *Attr, PrintLabel label) {
    printCommon(Attr, "custom_attr", label);
    printRec(Attr->getTypeRepr(), PrintLabel::suppressable("type_repr"));
    if (Attr->getArgs())
      printRec(Attr->getArgs(), PrintLabel::suppressable("args"));
    printFoot();
  }
  void visitDerivativeAttr(DerivativeAttr *Attr, PrintLabel label) {
    printCommon(Attr, "derivative_attr", label);
    printRec(Attr->getBaseTypeRepr(), PrintLabel::suppressable("type_repr"));
    printFieldRaw(
        [&](auto &out) { Attr->getOriginalFunctionName().Name.print(out); },
        "original_function_name");
    // TODO: Print parameters.
    printFoot();
  }
  void visitDifferentiableAttr(DifferentiableAttr *Attr, PrintLabel label) {
    printCommon(Attr, "differentiable_attr", label);
    // TODO: Implement.
    printFoot();
  }
  void visitDocumentationAttr(DocumentationAttr *Attr, PrintLabel label) {
    printCommon(Attr, "documentation_attr", label);
    printFieldQuoted(Attr->Metadata, "metadata");
    if (Attr->Visibility.has_value())
      printField(Attr->Visibility.value(), "visibility");
    printFoot();
  }
  void visitDynamicReplacementAttr(DynamicReplacementAttr *Attr,
                                   PrintLabel label) {
    printCommon(Attr, "dynamic_replacement_attr", label);
    printFieldRaw(
        [&](auto &out) { Attr->getReplacedFunctionName().print(out); },
        "replaced_function_name");
    printFoot();
  }
  void visitEffectsAttr(EffectsAttr *Attr, PrintLabel label) {
    printCommon(Attr, "effects_attr", label);
    printField(Attr->getKind(), "kind");
    if (Attr->getKind() == EffectsKind::Custom) {
      printFieldQuoted(Attr->getCustomString(), "custom");
    }
    printFoot();
  }
  void visitExclusivityAttr(ExclusivityAttr *Attr, PrintLabel label) {
    printCommon(Attr, "exclusivity_attr", label);
    printField(Attr->getMode(), "mode");
    printFoot();
  }
  void visitExposeAttr(ExposeAttr *Attr, PrintLabel label) {
    printCommon(Attr, "expose_attr", label);
    printFieldQuoted(Attr->Name, "name");
    printFoot();
  }
  void visitExternAttr(ExternAttr *Attr, PrintLabel label) {
    printCommon(Attr, "extern_attr", label);
    printField(Attr->getExternKind(), "kind");
    if (Attr->ModuleName.has_value())
      printField(Attr->ModuleName.value(), "module");
    printFieldQuoted(Attr->Name, "name");
    printFoot();
  }
  void visitImplementsAttr(ImplementsAttr *Attr, PrintLabel label) {
    printCommon(Attr, "implements_attr", label);
    printRec(Attr->getProtocolTypeRepr(), "protocol");
    printFieldRaw([&](auto &out) { Attr->getMemberName().print(out); },
                  "member");
    printFoot();
  }
  void visitInlineAttr(InlineAttr *Attr, PrintLabel label) {
    printCommon(Attr, "inline_attr", label);
    printField(Attr->getKind(), "kind");
    printFoot();
  }
  void visitLifetimeAttr(LifetimeAttr *Attr, PrintLabel label) {
    printCommon(Attr, "lifetime_attr", label);
    // TODO: Implement.
    printFoot();
  }
  void visitMacroRoleAttr(MacroRoleAttr *Attr, PrintLabel label) {
    printCommon(Attr, "macro_role_attr", label);
    switch (Attr->getMacroSyntax()) {
    case MacroSyntax::Attached:
      printFlag("attached");
      break;
    case MacroSyntax::Freestanding:
      printFlag("freestanding");
      break;
    }
    printField(Attr->getMacroRole(), "role");
    printFieldQuotedRaw(
        [&](auto &out) {
          llvm::interleave(
              Attr->getNames(), out,
              [&](const MacroIntroducedDeclName &name) {
                out << getMacroIntroducedDeclNameString(name.getKind());
                if (macroIntroducedNameRequiresArgument(name.getKind())) {
                  out << "(";
                  name.getName().print(out);
                  out << ")";
                }
              },
              ",");
        },
        "names");
    printRecRange(Attr->getConformances(), "conformances");

    printFoot();
  }
  void visitNonSendableAttr(NonSendableAttr *Attr, PrintLabel label) {
    printCommon(Attr, "non_sendable_attr", label);
    printField(Attr->Specificity, "specificity");
    printFoot();
  }
  void visitNonisolatedAttr(NonisolatedAttr *Attr, PrintLabel label) {
    printCommon(Attr, "nonisolated_attr", label);
    printFlag(Attr->isUnsafe(), "unsafe");
    printFoot();
  }
  void visitObjCAttr(ObjCAttr *Attr, PrintLabel label) {
    printCommon(Attr, "objc_attr", label);
    if (Attr->hasName())
      printFieldQuoted(Attr->getName(), "name");
    printFlag(Attr->isNameImplicit(), "is_name_implicit");
    printFoot();
  }
  void visitObjCBridgedAttr(ObjCBridgedAttr *Attr, PrintLabel label) {
    printCommon(Attr, "objc_bridged_attr", label);
    printDeclRefField(Attr->getObjCClass(), "objc_class");
    printFoot();
  }
  void visitObjCImplementationAttr(ObjCImplementationAttr *Attr,
                                   PrintLabel label) {
    printCommon(Attr, "objc_implementation_attr", label);
    if (!Attr->CategoryName.empty())
      printField(Attr->CategoryName, "category");
    printFlag(Attr->isEarlyAdopter(), "is_early_adopter");
    printFlag(Attr->isCategoryNameInvalid(), "is_category_name_invalid");
    printFlag(Attr->hasInvalidImplicitLangAttrs(),
              "has_invalid_implicit_lang_attrs");
    printFoot();
  }
  void visitObjCRuntimeNameAttr(ObjCRuntimeNameAttr *Attr, PrintLabel label) {
    printCommon(Attr, "objc_runtime_name_attr", label);
    printField(Attr->Name, "name");
    printFoot();
  }
  void visitOptimizeAttr(OptimizeAttr *Attr, PrintLabel label) {
    printCommon(Attr, "optimize_attr", label);
    printField(Attr->getMode(), "mode");
    printFoot();
  }
  void visitOriginallyDefinedInAttr(OriginallyDefinedInAttr *Attr,
                                    PrintLabel label) {
    printCommon(Attr, "originally_defined_in_attr", label);
    printField(Attr->OriginalModuleName, "original_module");
    printField(Attr->Platform, "platform");
    printFieldRaw([&](auto &out) { out << Attr->MovedVersion.getAsString(); },
                  "moved_version");
    printFoot();
  }
  void visitPrivateImportAttr(PrivateImportAttr *Attr, PrintLabel label) {
    printCommon(Attr, "prinvate_import_attr", label);
    printFieldQuoted(Attr->getSourceFile(), "source_file");
    printFoot();
  }
  void visitProjectedValuePropertyAttr(ProjectedValuePropertyAttr *Attr,
                                       PrintLabel label) {
    printCommon(Attr, "projected_value_property_attr", label);
    printField(Attr->ProjectionPropertyName, "name");
    printFoot();
  }
  void visitRawDocCommentAttr(RawDocCommentAttr *Attr, PrintLabel label) {
    printCommon(Attr, "raw_doc_comment_attr", label);
    printFieldRaw(
        [&](auto &out) { Attr->getCommentRange().print(out, Ctx->SourceMgr); },
        "comment_range");
    printFoot();
  }
  void visitRawLayoutAttr(RawLayoutAttr *Attr, PrintLabel label) {
    printCommon(Attr, "raw_layout_attr", label);
    if (auto *tyR = Attr->getScalarLikeType()) {
      printFlag("scalar_like");
      printRec(tyR, PrintLabel::suppressable("scalar_like_type"));
    } else if (auto typeAndCount = Attr->getArrayLikeTypeAndCount()) {
      printFlag("array_like");
      printRec(typeAndCount->first, PrintLabel::suppressable("array_like_type"));
      printRec(typeAndCount->second, PrintLabel::suppressable("count"));
    } else if (auto sizeAndAlignment = Attr->getSizeAndAlignment()) {
      printField(sizeAndAlignment->first, "size");
      printField(sizeAndAlignment->second, "alignment");
    }
    printFoot();
  }
  void visitReferenceOwnershipAttr(ReferenceOwnershipAttr *Attr,
                                   PrintLabel label) {
    printCommon(Attr, "reference_ownership_attr", label);
    printFlag(keywordOf(Attr->get()));
    printFoot();
  }
  void visitRestatedObjCConformanceAttr(RestatedObjCConformanceAttr *Attr,
                                        PrintLabel label) {
    printCommon(Attr, "restated_objc_conformance_attr", label);
    if (Attr->Proto) {
      printFieldRaw([&](auto &out) { Attr->Proto->dumpRef(out); }, "");
    }
    printFoot();
  }
  void visitSafeAttr(SafeAttr *Attr, PrintLabel label) {
    printCommon(Attr, "safe_attr", label);
    printFieldQuoted(Attr->message, "message");
    printFoot();
  }
  void visitSILGenNameAttr(SILGenNameAttr *Attr, PrintLabel label) {
    printCommon(Attr, "silgen_name_attr", label);
    printFlag(Attr->Raw, "raw");
    printFieldQuoted(Attr->Name, "");
    printFoot();
  }
  void visitSPIAccessControlAttr(SPIAccessControlAttr *Attr, PrintLabel label) {
    printCommon(Attr, "spi_access_control_attr", label);
    printFieldQuotedRaw(
        [&](auto &out) { llvm::interleave(Attr->getSPIGroups(), out, ","); },
        "groups");
    printFoot();
  }
  void visitSectionAttr(SectionAttr *Attr, PrintLabel label) {
    printCommon(Attr, "section_attr", label);
    printFieldQuoted(Attr->Name, "name");
    printFoot();
  }
  void visitSemanticsAttr(SemanticsAttr *Attr, PrintLabel label) {
    printCommon(Attr, "semantics_attr", label);
    printFieldQuoted(Attr->Value, "value");
    printFoot();
  }
  void visitSetterAccessAttr(SetterAccessAttr *Attr, PrintLabel label) {
    printCommon(Attr, "setter_access_attr", label);
    printField(Attr->getAccess(), "access");
    printFoot();
  }
  void visitSpecializeAttr(SpecializeAttr *Attr, PrintLabel label) {
    printCommon(Attr, "specialize_attr", label);
    printFlag(Attr->isExported(), "exported");
    printFlag(Attr->isFullSpecialization(), "full");
    printFlag(Attr->isPartialSpecialization(), "partial");
    if (Attr->getTargetFunctionName()) {
      printFieldQuotedRaw(
          [&](auto &out) { Attr->getTargetFunctionName().print(out); },
          "target");
    }
    if (!Attr->getSPIGroups().empty()) {
      printFieldQuotedRaw(
          [&](auto &out) { llvm::interleave(Attr->getSPIGroups(), out, ","); },
          "spi");
    }
    if (Attr->getTrailingWhereClause()) {
      printFieldQuotedRaw(
          [&](auto &out) {
            Attr->getTrailingWhereClause()->print(out,
                                                  /*printWhereKeyword=*/false);
          },
          "requirements");
    }
    printList("available_attrs", Attr->getAvailableAttrs(),
              [&](auto *availableAttr) { printRec(availableAttr, Ctx); });
    printFoot();
  }
  void visitStorageRestrictionsAttr(StorageRestrictionsAttr *Attr,
                                    PrintLabel label) {
    printCommon(Attr, "storage_restrictions_attr", label);
    if (!Attr->getInitializesNames().empty()) {
      printFieldQuotedRaw(
          [&](auto &out) {
            llvm::interleave(Attr->getInitializesNames(), out, ",");
          },
          "initializes");
    }
    if (!Attr->getAccessesNames().empty()) {
      printFieldQuotedRaw(
          [&](auto &out) {
            llvm::interleave(Attr->getAccessesNames(), out, ",");
          },
          "accesses");
    }
    printFoot();
  }
  void visitSwiftNativeObjCRuntimeBaseAttr(SwiftNativeObjCRuntimeBaseAttr *Attr,
                                           PrintLabel label) {
    printCommon(Attr, "swift_native_objc_runtime_base", label);
    printFieldQuoted(Attr->BaseClassName, "base_class_name");
    printFoot();
  }
  void visitSynthesizedProtocolAttr(SynthesizedProtocolAttr *Attr,
                                    PrintLabel label) {
    printCommon(Attr, "synthesized_protocol_attr", label);
    printFlag(Attr->isUnchecked(), "unchecked");
    printFieldQuotedRaw([&](auto &out) { Attr->getProtocol()->dumpRef(out); },
                        "protocol");
    printFoot();
  }
  void visitTransposeAttr(TransposeAttr *Attr, PrintLabel label) {
    printCommon(Attr, "transpose_attr", label);
    // TODO: Implement.
    printFoot();
  }
  void visitTypeEraserAttr(TypeEraserAttr *Attr, PrintLabel label) {
    printCommon(Attr, "type_eraser_attr", label);
    printFieldQuoted(Attr->getTypeWithoutResolving(), "type");
    printRec(Attr->getParsedTypeEraserTypeRepr(), "parsed_type_repr");
    printFoot();
  }
  void visitUnavailableFromAsyncAttr(UnavailableFromAsyncAttr *Attr,
                                     PrintLabel label) {
    printCommon(Attr, "unavailable_from_async_attr", label);
    if (Attr->hasMessage()) {
      printFieldQuoted(Attr->Message, "message");
    }
    printFoot();
  }
};

} // end anonymous namespace

void PrintBase::printRec(Decl *D, PrintLabel label) {
  printRecArbitrary([&](PrintLabel label) {
    if (!D) {
      printHead("<null decl>", DeclColor, label);
      printFoot();
    } else {
      PrintDecl(Writer, ParseIfNeeded, GetTypeOfExpr, GetTypeOfTypeRepr,
                GetTypeOfKeyPathComponent)
          .visit(D, label);
    }
  }, label);
}
void PrintBase::printRec(Expr *E, PrintLabel label) {
  printRecArbitrary([&](PrintLabel label) {
    if (!E) {
      printHead("<null expr>", ExprColor, label);
      printFoot();
    } else {
      PrintExpr(Writer, ParseIfNeeded, GetTypeOfExpr, GetTypeOfTypeRepr,
                GetTypeOfKeyPathComponent)
          .visit(E, label);
    }
  }, label);
}
void PrintBase::printRec(Stmt *S, const ASTContext *Ctx, PrintLabel label) {
  printRecArbitrary([&](PrintLabel label) {
    if (!S) {
      printHead("<null stmt>", ExprColor, label);
      printFoot();
    } else {
      PrintStmt(Writer, Ctx, ParseIfNeeded, GetTypeOfExpr,
                GetTypeOfTypeRepr, GetTypeOfKeyPathComponent)
          .visit(S, label);
    }
  }, label);
}
void PrintBase::printRec(TypeRepr *T, PrintLabel label) {
  printRecArbitrary([&](PrintLabel label) {
    if (!T) {
      printHead("<null typerepr>", TypeReprColor, label);
      printFoot();
    } else {
      PrintTypeRepr(Writer, ParseIfNeeded, GetTypeOfExpr, GetTypeOfTypeRepr,
                    GetTypeOfKeyPathComponent)
          .visit(T, label);
    }
  }, label);
}
void PrintBase::printRec(const Pattern *P, PrintLabel label) {
  printRecArbitrary([&](PrintLabel label) {
    if (!P) {
      printHead("<null pattern>", PatternColor, label);
      printFoot();
    } else {
      PrintPattern(Writer, ParseIfNeeded, GetTypeOfExpr, GetTypeOfTypeRepr,
                   GetTypeOfKeyPathComponent)
          .visit(const_cast<Pattern *>(P), label);
    }
  }, label);
}
void PrintBase::printRec(const DeclAttribute *Attr, const ASTContext *Ctx,
                         PrintLabel label) {
  printRecArbitrary(
      [&](PrintLabel label) {
        if (!Attr) {
          printHead("<null attribute>", DeclAttributeColor, label);
          printFoot();
        } else {
          PrintAttribute(Writer, Ctx, ParseIfNeeded, GetTypeOfExpr,
                         GetTypeOfTypeRepr, GetTypeOfKeyPathComponent)
              .visit(const_cast<DeclAttribute *>(Attr), label);
        }
      },
      label);
}

void TypeRepr::dump() const {
  dump(llvm::errs());
  llvm::errs() << '\n';
}
void TypeRepr::dump(raw_ostream &os, unsigned indent) const {
  DefaultWriter writer(os, indent);
  PrintTypeRepr(writer).visit(const_cast<TypeRepr*>(this), "");
}

namespace {

class PrintConformance : public PrintBase {
public:
  using PrintBase::PrintBase;

  void visitProtocolConformanceRef(const ProtocolConformanceRef conformance,
                                   VisitedConformances &visited,
                                   PrintLabel label) {
    if (conformance.isInvalid()) {
      printHead("invalid_conformance", ASTNodeColor, label);
      printFoot();
    } else if (conformance.isConcrete()) {
      visitProtocolConformance(conformance.getConcrete(), visited, label);
    } else if (conformance.isPack()) {
      visitPackConformance(conformance.getPack(), visited, label);
    } else {
      assert(conformance.isAbstract());

      printHead("abstract_conformance", ASTNodeColor, label);
      printFieldQuoted(conformance.getAbstract()->getName(), "protocol");
      printFoot();
    }
  }

  void visitProtocolConformance(const ProtocolConformance *conformance,
                                VisitedConformances &visited, PrintLabel label) {
    // A recursive conformance shouldn't have its contents printed, or there's
    // infinite recursion. (This also avoids printing things that occur multiple
    // times in a conformance hierarchy.)
    auto shouldPrintDetails = visited.insert(conformance).second;

    auto printCommon = [&](StringRef kind) {
      printHead(kind, ASTNodeColor, label);
      printFieldQuoted(conformance->getType(), "type");
      printFieldQuoted(conformance->getProtocol()->getName(), "protocol");
      printFlag(!shouldPrintDetails, "<details printed above>");
    };

    switch (conformance->getKind()) {
      case ProtocolConformanceKind::Normal: {
        auto normal = cast<NormalProtocolConformance>(conformance);

        printCommon("normal_conformance");
        if (!shouldPrintDetails)
          break;

        // Maybe print information about the conforming context?
        if (normal->isLazilyLoaded()) {
          printFlag("lazy");
        } else {
          printListArbitrary("typeWitnesses", [&]{
            normal->forEachTypeWitness([&](const AssociatedTypeDecl *req,
                                           Type ty, const TypeDecl *) -> bool {
              printRecArbitrary([&](PrintLabel label) {
                printHead("assoc_type", ASTNodeColor, label);
                printFieldQuoted(req->getName(), "req");
                printFieldQuoted(Type(ty->getDesugaredType()), "type",
                                 TypeColor);
                printFoot();
              });
              return false;
            });
          });
          printListArbitrary("valueWitnesses", [&]{
            normal->forEachValueWitness([&](const ValueDecl *req,
                                            Witness witness) {
              printRecArbitrary([&](PrintLabel label) {
                printHead("value", ASTNodeColor, label);
                printFieldQuoted(req->getName(), "req");
                if (!witness)
                  printFlag("no_witness");
                else if (witness.getDecl() == req)
                  printFlag("dynamic_witness");
                else
                  printFieldQuotedRaw([&](raw_ostream &out) {
                    witness.getDecl()->dumpRef(out);
                  }, "witness");
                printFoot();
              });
            });
          });
          printListArbitrary("associatedConformances", [&]{
            normal->forEachAssociatedConformance(
                [&](Type t, ProtocolDecl *proto, unsigned index) {
                  printRecArbitrary([&](PrintLabel label) {
                    printHead("assoc_conformance", ASTNodeColor, label);
                    printFieldQuoted(t, "type", TypeColor);
                    printFieldQuoted(proto->getName(), "proto");
                    printRec(normal->getAssociatedConformance(t, proto), visited,
                             PrintLabel::suppressable("conformance"));
                    printFoot();
                  });
                  return false;
                });
          });
        }

        if (auto condReqs = normal->getConditionalRequirementsIfAvailable()) {
          printList("conditional_requirements", *condReqs, [&](auto requirement) {
            printRec(requirement);
          });
        } else {
          printRecArbitrary([&](PrintLabel label) {
            printHead("<conditional requirements unable to be computed>",
                      ASTNodeColor);
            printFoot();
          });
        }
        break;
      }

      case ProtocolConformanceKind::Self: {
        printCommon("self_conformance");
        break;
      }

      case ProtocolConformanceKind::Inherited: {
        auto conf = cast<InheritedProtocolConformance>(conformance);
        printCommon("inherited_conformance");
        if (!shouldPrintDetails)
          break;

        printRec(conf->getInheritedConformance(), visited);
        break;
      }

      case ProtocolConformanceKind::Specialized: {
        auto conf = cast<SpecializedProtocolConformance>(conformance);
        printCommon("specialized_conformance");
        if (!shouldPrintDetails)
          break;

        printRec(conf->getSubstitutionMap(), visited);
        if (auto condReqs = conf->getConditionalRequirementsIfAvailableOrCached(/*computeIfPossible=*/false)) {
          printList("conditional_requirements", *condReqs, [&](auto subReq) {
            printRec(subReq);
          });
        } else {
          printRecArbitrary([&](PrintLabel label) {
            printHead("<conditional requirements unable to be computed>",
                      ASTNodeColor);
            printFoot();
          });
        }
        printRec(conf->getGenericConformance(), visited);
        break;
      }

      case ProtocolConformanceKind::Builtin: {
        printCommon("builtin_conformance");
      }
    }

    printFoot();
  }

  void visitPackConformance(const PackConformance *conformance,
                            VisitedConformances &visited, PrintLabel label) {
    printHead("pack_conformance", ASTNodeColor, label);

    printFieldQuoted(Type(conformance->getType()), "type");
    printFieldQuoted(conformance->getProtocol()->getName(), "protocol");

    printList("pattern_conformances", conformance->getPatternConformances(),
              [&](auto conformanceRef) { printRec(conformanceRef, visited); });

    printFoot();
  }

  void visitSubstitutionMap(SubstitutionMap map,
                            SubstitutionMap::DumpStyle style,
                            VisitedConformances &visited, PrintLabel label) {
    // In Minimal style, use single quote so this dump can appear in
    // double-quoted fields without escaping.
    char quote = Writer.quote();
    if (style == SubstitutionMap::DumpStyle::Minimal)
      Writer.setQuote('\'');
    SWIFT_DEFER { Writer.setQuote(quote); };

    auto genericSig = map.getGenericSignature();
    printHead("substitution_map", ASTNodeColor, label);
    SWIFT_DEFER { printFoot(); };

    if (genericSig.isNull()) {
      printFlag("null_generic_signature");
      return;
    }

    printFieldRaw([&](raw_ostream &out) { genericSig->print(out); },
                  "generic_signature");

    auto genericParams = genericSig.getGenericParams();
    auto replacementTypes =
    static_cast<const SubstitutionMap &>(map).getReplacementTypes();
    for (unsigned i : indices(genericParams)) {
      if (style == SubstitutionMap::DumpStyle::Minimal) {
        printFieldRaw([&](raw_ostream &out) {
          genericParams[i]->print(out);
          out << " -> ";
          out << replacementTypes[i];
        }, "");
      } else {
        printRecArbitrary([&](PrintLabel label) {
          printHead("substitution", ASTNodeColor, label);
          printFieldRaw([&](raw_ostream &out) {
            genericParams[i]->print(out);
            out << " -> ";
          }, "");
          printRec(replacementTypes[i]);
          printFoot();
        });
      }
    }

    // A minimal dump doesn't need the details about the conformances, a lot of
    // that info can be inferred from the signature.
    if (style == SubstitutionMap::DumpStyle::Minimal)
      return;

    auto conformances = map.getConformances();
    for (const auto &req : genericSig.getRequirements()) {
      if (req.getKind() != RequirementKind::Conformance)
        continue;

      printRecArbitrary([&](PrintLabel label) {
        printHead("conformance", ASTNodeColor, label);
        printFieldQuoted(req.getFirstType(), "type");
        printRec(conformances.front(), visited);
        printFoot();
      });
      conformances = conformances.slice(1);
    }
  }
};

void PrintBase::printRec(SubstitutionMap map, VisitedConformances &visited,
                         PrintLabel label) {
  printRecArbitrary([&](PrintLabel label) {
    PrintConformance(Writer)
        .visitSubstitutionMap(map, SubstitutionMap::DumpStyle::Full, visited,
                              label);
  }, label);
}

void PrintBase::printRec(const ProtocolConformanceRef &ref,
                         VisitedConformances &visited, PrintLabel label) {
  printRecArbitrary([&](PrintLabel label) {
    PrintConformance(Writer)
          .visitProtocolConformanceRef(ref, visited, label);
  }, label);
}

void PrintBase::printRec(const ProtocolConformance *conformance,
                         VisitedConformances &visited, PrintLabel label) {
  printRecArbitrary([&](PrintLabel label) {
    PrintConformance(Writer)
        .visitProtocolConformance(conformance, visited, label);
  }, label);
}

} // end anonymous namespace

void ProtocolConformanceRef::dump() const {
  dump(llvm::errs());
  llvm::errs() << '\n';
}

void ProtocolConformanceRef::dump(llvm::raw_ostream &out, unsigned indent,
                                  bool details) const {
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  if (!details && isConcrete())
    visited.insert(getConcrete());

  DefaultWriter writer(out, indent);
  PrintConformance(writer).visitProtocolConformanceRef(*this, visited, "");
}

void ProtocolConformanceRef::print(llvm::raw_ostream &out) const {
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  DefaultWriter writer(out, /*indent*/ 0);
  PrintConformance(writer).visitProtocolConformanceRef(*this, visited, "");
}

void ProtocolConformance::dump() const {
  auto &out = llvm::errs();
  dump(out);
  out << '\n';
}

void ProtocolConformance::dump(llvm::raw_ostream &out, unsigned indent) const {
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  DefaultWriter writer(out, indent);
  PrintConformance(writer).visitProtocolConformance(this, visited, "");
}

void PackConformance::dump(llvm::raw_ostream &out, unsigned indent) const {
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  DefaultWriter writer(out, indent);
  PrintConformance(writer).visitPackConformance(this, visited, "");
}

void SubstitutionMap::dump(llvm::raw_ostream &out, DumpStyle style,
                           unsigned indent) const {
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  DefaultWriter writer(out, indent);
  PrintConformance(writer).visitSubstitutionMap(*this, style, visited, "");
}

void SubstitutionMap::dump() const {
  dump(llvm::errs());
  llvm::errs() << "\n";
}

//===----------------------------------------------------------------------===//
// Dumping for Types.
//===----------------------------------------------------------------------===//

namespace {
  class PrintType : public TypeVisitor<PrintType, void, PrintLabel>,
                    public PrintBase {
    void printCommon(StringRef name, PrintLabel label) {
      printHead(name, TypeColor, label);
    }

    void dumpParameterFlags(ParameterTypeFlags paramFlags) {
      printFlag(paramFlags.isVariadic(), "vararg");
      printFlag(paramFlags.isAutoClosure(), "autoclosure");
      printFlag(paramFlags.isNonEphemeral(), "nonEphemeral");
      printFlag(paramFlags.isCompileTimeConst(), "compileTimeConst");
      printFlag(getDumpString(paramFlags.getValueOwnership()));
    }

  public:
    using PrintBase::PrintBase;

#define TRIVIAL_TYPE_PRINTER(Class,Name)                        \
    void visit##Class##Type(Class##Type *T, PrintLabel label) {  \
      printCommon(#Name "_type", label); printFoot();           \
    }

    void visitErrorType(ErrorType *T, PrintLabel label) {
      printCommon("error_type", label);
      if (auto originalType = T->getOriginalType())
        printRec(originalType, "original_type");
      printFoot();
    }

    TRIVIAL_TYPE_PRINTER(Unresolved, unresolved)

    void visitPlaceholderType(PlaceholderType *T, PrintLabel label) {
      printCommon("placeholder_type", label);
      auto originator = T->getOriginator();
      if (auto *typeVar = originator.dyn_cast<TypeVariableType *>()) {
        printRec(typeVar, "type_variable");
      } else if (auto *VD = originator.dyn_cast<VarDecl *>()) {
        printFieldQuotedRaw([&](raw_ostream &OS) { VD->dumpRef(OS); }, "",
                            DeclColor);
      } else if (auto *EE = originator.dyn_cast<ErrorExpr *>()) {
        printFlag("error_expr");
      } else if (auto *DMT = originator.dyn_cast<DependentMemberType *>()) {
        printRec(DMT, "dependent_member_type");
      } else if (originator.is<TypeRepr *>()) {
        printFlag("type_repr");
      } else {
        assert(false && "unknown originator");
      }
      printFoot();
    }

    void visitBuiltinIntegerType(BuiltinIntegerType *T, PrintLabel label) {
      printCommon("builtin_integer_type", label);
      if (T->isFixedWidth())
        printField(T->getFixedWidth(), "bit_width");
      else
        printFlag("word_sized");
      printFoot();
    }

    void visitBuiltinFloatType(BuiltinFloatType *T, PrintLabel label) {
      printCommon("builtin_float_type", label);
      printField(T->getBitWidth(), "bit_width");
      printFoot();
    }

    TRIVIAL_TYPE_PRINTER(BuiltinIntegerLiteral, builtin_integer_literal)
    TRIVIAL_TYPE_PRINTER(BuiltinJob, builtin_job)
    TRIVIAL_TYPE_PRINTER(BuiltinExecutor, builtin_executor_ref)
    TRIVIAL_TYPE_PRINTER(BuiltinDefaultActorStorage, builtin_default_actor_storage)
    TRIVIAL_TYPE_PRINTER(BuiltinNonDefaultDistributedActorStorage, builtin_non_default_distributed_actor_storage)
    TRIVIAL_TYPE_PRINTER(BuiltinPackIndex, builtin_pack_index)
    TRIVIAL_TYPE_PRINTER(BuiltinRawPointer, builtin_raw_pointer)
    TRIVIAL_TYPE_PRINTER(BuiltinRawUnsafeContinuation, builtin_raw_unsafe_continuation)
    TRIVIAL_TYPE_PRINTER(BuiltinNativeObject, builtin_native_object)
    TRIVIAL_TYPE_PRINTER(BuiltinBridgeObject, builtin_bridge_object)
    TRIVIAL_TYPE_PRINTER(BuiltinUnsafeValueBuffer, builtin_unsafe_value_buffer)
    TRIVIAL_TYPE_PRINTER(SILToken, sil_token)

    void visitBuiltinVectorType(BuiltinVectorType *T, PrintLabel label) {
      printCommon("builtin_vector_type", label);
      printField(T->getNumElements(), "num_elements");
      printRec(T->getElementType());
      printFoot();
    }
    
    void visitBuiltinUnboundGenericType(BuiltinUnboundGenericType *T,
                                        PrintLabel label) {
      printCommon("builtin_unbound_generic_type", label);
      printField(T->getBuiltinTypeNameString(), "name");
      printFoot();
    }
    
    void visitBuiltinFixedArrayType(BuiltinFixedArrayType *T,
                                    PrintLabel label) {
      printCommon("builtin_fixed_array_type", label);
      printRec(T->getSize());
      printRec(T->getElementType());
      printFoot();
    }

    void visitTypeAliasType(TypeAliasType *T, PrintLabel label) {
      printCommon("type_alias_type", label);

      printFieldQuoted(T->getDecl()->printRef(), "decl");
      if (auto underlying = T->getSinglyDesugaredType()) {
        printRec(underlying, "underlying");
      } else {
        // This can't actually happen
        printFlag("unresolved_underlying");
      }

      if (T->getParent())
        printRec(T->getParent(), "parent");
      for (auto arg : T->getDirectGenericArgs())
        printRec(arg);

      printFoot();
    }

    void visitLocatableType(LocatableType *T, PrintLabel label) {
      printCommon("locatable_type", label);
      printFieldQuotedRaw(
          [&](raw_ostream &OS) {
            auto &C = T->getASTContext();
            T->getLoc().print(OS, C.SourceMgr);
          },
          "loc");
      printRec(T->getSinglyDesugaredType(), "underlying");
      printFoot();
    }

    void visitPackType(PackType *T, PrintLabel label) {
      printCommon("pack_type", label);

      printField(T->getNumElements(), "num_elements");

      for (Type elt : T->getElementTypes()) {
        printRec(elt);
      }

      printFoot();
    }

    void visitSILPackType(SILPackType *T, PrintLabel label) {
      printCommon("sil_pack_type", label);

      printField(T->isElementAddress(), "element_is_address");
      printField(T->getNumElements(), "num_elements");

      for (Type elt : T->getElementTypes()) {
        printRec(elt);
      }

      printFoot();
    }

    void visitPackExpansionType(PackExpansionType *T, PrintLabel label) {
      printCommon("pack_expansion_type", label);
      printRec(T->getPatternType(), "pattern");
      printRec(T->getCountType(), "count");
      printFoot();
    }

    void visitPackElementType(PackElementType *T, PrintLabel label) {
      printCommon("element_type", label);

      printField(T->getLevel(), "level");

      printRec(T->getPackType(), "pack");

      printFoot();
    }

    void visitTupleType(TupleType *T, PrintLabel label) {
      printCommon("tuple_type", label);

      printField(T->getNumElements(), "num_elements");

      for (const auto &elt : T->getElements()) {
        printRecArbitrary([&](PrintLabel label) {
          printHead("tuple_type_elt", FieldLabelColor, label);
          if (elt.hasName())
            printFieldQuoted(elt.getName().str(), "name");
          printRec(elt.getType());
          printFoot();
        });
      }

      printFoot();
    }

#define REF_STORAGE(Name, name, ...) \
    void visit##Name##StorageType(Name##StorageType *T, PrintLabel label) { \
      printCommon(#name "_storage_type", label); \
      printRec(T->getReferentType()); \
      printFoot(); \
    }
#include "swift/AST/ReferenceStorage.def"

#define VISIT_NOMINAL_TYPE(TypeClass, Name)                \
    void visit##TypeClass(TypeClass *T, PrintLabel label) { \
      printCommon(#Name, label);                           \
                                                           \
      printFieldQuoted(T->getDecl()->printRef(), "decl");  \
                                                           \
      if (T->getParent())                                  \
        printRec(T->getParent(), "parent");                \
                                                           \
      printFoot();                                         \
    }

    VISIT_NOMINAL_TYPE(EnumType, enum_type)
    VISIT_NOMINAL_TYPE(StructType, struct_type)
    VISIT_NOMINAL_TYPE(ClassType, class_type)
    VISIT_NOMINAL_TYPE(ProtocolType, protocol_type)

#undef VISIT_NOMINAL_TYPE

    void visitBuiltinTupleType(BuiltinTupleType *T, PrintLabel label) {
      printCommon("builtin_tuple_type", label);
      printFieldQuoted(T->getDecl()->printRef(), "decl");
      printFoot();
    }

    void visitMetatypeType(MetatypeType *T, PrintLabel label) {
      printCommon("metatype_type", label);

      if (T->hasRepresentation())
        printFlag(getDumpString(T->getRepresentation()));

      printRec(T->getInstanceType());

      printFoot();
    }

    void visitExistentialMetatypeType(ExistentialMetatypeType *T,
                                      PrintLabel label) {
      printCommon("existential_metatype_type", label);

      if (T->hasRepresentation())
        printFlag(getDumpString(T->getRepresentation()));

      printRec(T->getInstanceType());

      printFoot();
    }

    void visitModuleType(ModuleType *T, PrintLabel label) {
      printCommon("module_type", label);
      printDeclName(T->getModule(), "module");
      printFoot();
    }

    void visitDynamicSelfType(DynamicSelfType *T, PrintLabel label) {
      printCommon("dynamic_self_type", label);
      printRec(T->getSelfType());
      printFoot();
    }
    
    void printArchetypeCommon(ArchetypeType *T,
                              StringRef className,
                              PrintLabel label) {
      printCommon(className, label);

      printField(static_cast<void *>(T), "address");
      printFlag(T->requiresClass(), "class");
      if (auto layout = T->getLayoutConstraint()) {
        printFieldRaw([&](raw_ostream &OS) {
          layout->print(OS);
        }, "layout");
      }
      for (auto proto : T->getConformsTo())
        printFieldQuoted(proto->printRef(), "conforms_to");
    }

    void printArchetypeCommonRec(ArchetypeType *T) {
      printRec(T->getInterfaceType(), "interface_type");
      if (auto superclass = T->getSuperclass())
        printRec(superclass, "superclass");
    }

    void visitPrimaryArchetypeType(PrimaryArchetypeType *T, PrintLabel label) {
      printArchetypeCommon(T, "primary_archetype_type", label);

      printFieldQuoted(T->getFullName(), "name");

      printArchetypeCommonRec(T);

      printFoot();
    }
    void visitOpenedArchetypeType(OpenedArchetypeType *T, PrintLabel label) {
      printArchetypeCommon(T, "opened_archetype_type", label);

      auto *env = T->getGenericEnvironment();
      printFieldQuoted(env->getOpenedExistentialUUID(), "opened_existential_id");

      printArchetypeCommonRec(T);
      printRec(env->getOpenedExistentialType(), "opened_existential");
      if (auto subMap = env->getOuterSubstitutions())
        printRec(subMap, "substitutions");

      printFoot();
    }
    void visitOpaqueTypeArchetypeType(OpaqueTypeArchetypeType *T,
                                      PrintLabel label) {
      printArchetypeCommon(T, "opaque_type", label);

      printFieldQuoted(T->getDecl()->getNamingDecl()->printRef(), "decl");

      printArchetypeCommonRec(T);
      if (!T->getSubstitutions().empty()) {
        printRec(T->getSubstitutions());
      }

      printFoot();
    }
    void visitPackArchetypeType(PackArchetypeType *T, PrintLabel label) {
      printArchetypeCommon(T, "pack_archetype_type", label);
      printFieldQuoted(T->getFullName(), "name");
      printArchetypeCommonRec(T);
      printFoot();
    }
    void visitElementArchetypeType(ElementArchetypeType *T, PrintLabel label) {
      printArchetypeCommon(T, "element_archetype_type", label);
      printFieldQuoted(T->getOpenedElementID(), "opened_element_id");
      printFoot();
    }

    void visitGenericTypeParamType(GenericTypeParamType *T, PrintLabel label) {
      printCommon("generic_type_param_type", label);
      printField(T->getDepth(), "depth");
      printField(T->getIndex(), "index");
      if (!T->isCanonical())
        printFieldQuoted(T->getName(), "name");

      switch (T->getParamKind()) {
      case GenericTypeParamKind::Type:
        printField((StringRef)"type", "param_kind");
        break;
      case GenericTypeParamKind::Pack:
        printField((StringRef)"pack", "param_kind");
        break;
      case GenericTypeParamKind::Value:
        printField((StringRef)"value", "param_kind");
        printRec(T->getValueType(), "value_type");
      }

      printFoot();
    }

    void visitDependentMemberType(DependentMemberType *T, PrintLabel label) {
      printCommon("dependent_member_type", label);

      if (auto assocType = T->getAssocType()) {
        printFieldQuoted(assocType->printRef(), "assoc_type");
      } else {
        printFieldQuoted(T->getName(), "name");
      }

      printRec(T->getBase(), "base");

      printFoot();
    }

    void printAnyFunctionParamsRec(ArrayRef<AnyFunctionType::Param> params,
                                   PrintLabel label) {
      printRecArbitrary([&](PrintLabel label) {
        printCommon("function_params", label);

        printField(params.size(), "num_params");
        for (const auto &param : params) {
          printRecArbitrary([&](PrintLabel label) {
            printHead("param", FieldLabelColor, label);

            if (param.hasLabel())
              printFieldQuoted(param.getLabel().str(), "name");
            if (param.hasInternalLabel())
              printFieldQuoted(param.getInternalLabel().str(), "internal_name");
            dumpParameterFlags(param.getParameterFlags());

            printRec(param.getPlainType());

            printFoot();
          });
        }
        printFoot();
      }, label);
    }

    void printClangTypeRec(const ClangTypeInfo &info, const ASTContext &ctx) {
      // [TODO: Improve-Clang-type-printing]
      if (!info.empty()) {
        printRecArbitrary([&](PrintLabel label) {
          printHead("clang_type", ASTNodeColor, label);
          printNameRaw([&](raw_ostream &OS) {
            auto &clangCtx = ctx.getClangModuleLoader()->getClangASTContext();
            info.dump(OS, clangCtx);
          });
          printFoot();
        });
      }
    }

    void printAnyFunctionTypeCommonRec(AnyFunctionType *T, PrintLabel label,
                                    StringRef name) {
      printCommon(name, label);

      if (T->hasExtInfo()) {
        SILFunctionType::Representation representation =
            T->getExtInfo().getSILRepresentation();

        if (representation != SILFunctionType::Representation::Thick) {
          printField(representation, "representation");
        }
        printFlag(!T->isNoEscape(), "escaping");
        printFlag(T->isSendable(), "Sendable");
        printFlag(T->isAsync(), "async");
        printFlag(T->isThrowing(), "throws");
        printFlag(T->hasSendingResult(), "sending_result");
      }
      if (Type globalActor = T->getGlobalActor()) {
        printFieldQuoted(globalActor.getString(), "global_actor");
      }

      printClangTypeRec(T->getClangTypeInfo(), T->getASTContext());
      printAnyFunctionParamsRec(T->getParams(), "input");
      printRec(T->getResult(), "output");
      if (Type thrownError = T->getThrownError()) {
        printRec(thrownError, "thrown_error");
      }
    }

    void visitFunctionType(FunctionType *T, PrintLabel label) {
      printAnyFunctionTypeCommonRec(T, label, "function_type");
      printFoot();
    }

    void visitGenericFunctionType(GenericFunctionType *T, PrintLabel label) {
      printAnyFunctionTypeCommonRec(T, label, "generic_function_type");
      // FIXME: generic signature dumping needs improvement
      printRecArbitrary([&](PrintLabel label) {
        printHead("generic_sig", TypeColor, label);
        printFieldQuoted(T->getGenericSignature()->getAsString(), "");
        printFoot();
      });
      printFoot();
    }

    void visitSILFunctionType(SILFunctionType *T, PrintLabel label) {
      printCommon("sil_function_type", label);
      printFieldQuoted(T->getString(), "type");

      for (auto param : T->getParameters()) {
        printRec(param.getInterfaceType(), "input");
      }
      for (auto yield : T->getYields()) {
        printRec(yield.getInterfaceType(), "yield");
      }
      for (auto result : T->getResults()) {
        printRec(result.getInterfaceType(), "result");
      }
      if (auto error  = T->getOptionalErrorResult()) {
        printRec(error->getInterfaceType(), "error");
      }
      printRec(T->getPatternSubstitutions());
      printRec(T->getInvocationSubstitutions());
      printClangTypeRec(T->getClangTypeInfo(), T->getASTContext());

      printFoot();
    }

    void visitSILBlockStorageType(SILBlockStorageType *T, PrintLabel label) {
      printCommon("sil_block_storage_type", label);
      printRec(T->getCaptureType());
      printFoot();
    }

    void visitSILMoveOnlyWrappedType(SILMoveOnlyWrappedType *T,
                                     PrintLabel label) {
      printCommon("sil_move_only_type", label);
      printRec(T->getInnerType());
      printFoot();
    }

    void visitSILBoxType(SILBoxType *T, PrintLabel label) {
      printCommon("sil_box_type", label);
      // FIXME: Print the structure of the type.
      printFieldQuoted(T->getString(), "type");
      printFoot();
    }

    void visitArraySliceType(ArraySliceType *T, PrintLabel label) {
      printCommon("array_slice_type", label);
      printRec(T->getBaseType());
      printFoot();
    }

    void visitOptionalType(OptionalType *T, PrintLabel label) {
      printCommon("optional_type", label);
      printRec(T->getBaseType());
      printFoot();
    }

    void visitDictionaryType(DictionaryType *T, PrintLabel label) {
      printCommon("dictionary_type", label);
      printRec(T->getKeyType(), "key");
      printRec(T->getValueType(), "value");
      printFoot();
    }

    void visitVariadicSequenceType(VariadicSequenceType *T, PrintLabel label) {
      printCommon("variadic_sequence_type", label);
      printRec(T->getBaseType());
      printFoot();
    }

    void visitProtocolCompositionType(ProtocolCompositionType *T,
                                      PrintLabel label) {

      printCommon("protocol_composition_type", label);

      printFlag(T->hasExplicitAnyObject(), "any_object");

      for (auto ip : T->getInverses()) {
        switch (ip) {
        case InvertibleProtocolKind::Copyable:
          printFlag("inverse_copyable");
          break;
        case InvertibleProtocolKind::Escapable:
          printFlag("inverse_escapable");
          break;
        }
      }

      for (auto proto : T->getMembers()) {
        printRec(proto);
      }

      printFoot();
    }

    void visitParameterizedProtocolType(ParameterizedProtocolType *T,
                                        PrintLabel label) {
      printCommon("parameterized_protocol_type", label);
      printRec(T->getBaseType(), "base");
      for (auto arg : T->getArgs()) {
        printRec(arg);
      }
      printFoot();
    }

    void visitExistentialType(ExistentialType *T,
                              PrintLabel label) {
      printCommon("existential_type", label);
      printRec(T->getConstraintType());
      printFoot();
    }

    void visitLValueType(LValueType *T, PrintLabel label) {
      printCommon("lvalue_type", label);
      printRec(T->getObjectType());
      printFoot();
    }

    void visitInOutType(InOutType *T, PrintLabel label) {
      printCommon("inout_type", label);
      printRec(T->getObjectType());
      printFoot();
    }

    void visitUnboundGenericType(UnboundGenericType *T, PrintLabel label) {
      printCommon("unbound_generic_type", label);
      printFieldQuoted(T->getDecl()->printRef(), "decl");
      if (T->getParent())
        printRec(T->getParent(), "parent");
      printFoot();
    }

    void visitBoundGenericClassType(BoundGenericClassType *T, PrintLabel label) {
      printCommon("bound_generic_class_type", label);
      printFieldQuoted(T->getDecl()->printRef(), "decl");
      if (T->getParent())
        printRec(T->getParent(), "parent");
      for (auto arg : T->getGenericArgs())
        printRec(arg);
      printFoot();
    }

    void visitBoundGenericStructType(BoundGenericStructType *T,
                                     PrintLabel label) {
      printCommon("bound_generic_struct_type", label);
      printFieldQuoted(T->getDecl()->printRef(), "decl");
      if (T->getParent())
        printRec(T->getParent(), "parent");
      for (auto arg : T->getGenericArgs())
        printRec(arg);
      printFoot();
    }

    void visitBoundGenericEnumType(BoundGenericEnumType *T, PrintLabel label) {
      printCommon("bound_generic_enum_type", label);
      printFieldQuoted(T->getDecl()->printRef(), "decl");
      if (T->getParent())
        printRec(T->getParent(), "parent");
      for (auto arg : T->getGenericArgs())
        printRec(arg);
      printFoot();
    }

    void visitTypeVariableType(TypeVariableType *T, PrintLabel label) {
      printCommon("type_variable_type", label);
      printField(T->getID(), "id");
      printFoot();
    }

    void visitErrorUnionType(ErrorUnionType *T, PrintLabel label) {
      printCommon("error_union_type", label);
      for (auto term : T->getTerms())
        printRec(term);
      printFoot();
    }

    void visitIntegerType(IntegerType *T, PrintLabel label) {
      printCommon("integer_type", label);
      printFlag(T->isNegative(), "is_negative");
      printFieldQuoted(T->getValue(), "value", LiteralValueColor);
      printFoot();
    }

#undef TRIVIAL_TYPE_PRINTER
  };

  void PrintBase::printRec(Type type, PrintLabel label) {
    printRecArbitrary([&](PrintLabel label) {
      if (type.isNull()) {
        printHead("<null type>", DeclColor, label);
        printFoot();
      } else {
        PrintType(Writer, ParseIfNeeded, GetTypeOfExpr, GetTypeOfTypeRepr,
                  GetTypeOfKeyPathComponent)
            .visit(type, label);
      }
    }, label);
  }
} // end anonymous namespace

void Type::dump() const {
  dump(llvm::errs());
}

void Type::dump(raw_ostream &os, unsigned indent) const {
  DefaultWriter writer(os, indent);
  PrintType(writer).visit(*this, "");
  os << "\n";
}

void TypeBase::dump() const {
  // Make sure to print type variables.
  Type(const_cast<TypeBase *>(this)).dump();
}

void TypeBase::dump(raw_ostream &os, unsigned indent) const {
  Type(const_cast<TypeBase *>(this)).dump(os, indent);
}

void GenericSignatureImpl::dump() const {
  GenericSignature(const_cast<GenericSignatureImpl *>(this)).dump();
}

void GenericEnvironment::dump(raw_ostream &os) const {
  os << "Generic environment:\n";
  for (auto gp : getGenericParams()) {
    gp->dump(os);
    mapTypeIntoContext(gp)->dump(os);
  }
  os << "Generic parameters:\n";
  for (auto paramTy : getGenericParams())
    paramTy->dump(os);
}

void GenericEnvironment::dump() const {
  dump(llvm::errs());
}

StringRef swift::getAccessorKindString(AccessorKind value) {
  switch (value) {
#define ACCESSOR(ID)
#define SINGLETON_ACCESSOR(ID, KEYWORD) \
  case AccessorKind::ID: return #KEYWORD;
#include "swift/AST/AccessorKinds.def"
  }

  llvm_unreachable("Unhandled AccessorKind in switch.");
}

void StableSerializationPath::dump() const {
  dump(llvm::errs());
}

static StringRef getExternalPathComponentKindString(
                  StableSerializationPath::ExternalPath::ComponentKind kind) {
  switch (kind) {
#define CASE(ID, STRING) \
  case StableSerializationPath::ExternalPath::ID: return STRING;
  CASE(Record, "record")
  CASE(Enum, "enum")
  CASE(Namespace, "namespace")
  CASE(Typedef, "typedef")
  CASE(TypedefAnonDecl, "anonymous tag")
  CASE(ObjCInterface, "@interface")
  CASE(ObjCProtocol, "@protocol")
#undef CASE
  }
  llvm_unreachable("bad kind");
}

void StableSerializationPath::dump(llvm::raw_ostream &os) const {
  if (isSwiftDecl()) {
    os << "clang decl of:\n";
    getSwiftDecl()->dump(os, 2);
  } else {
    auto &path = getExternalPath();
    using ExternalPath = StableSerializationPath::ExternalPath;
    os << "external path: ";
    size_t index = 0;
    for (auto &entry : path.Path) {
      if (index++) os << " -> ";
      os << getExternalPathComponentKindString(entry.first);
      if (ExternalPath::requiresIdentifier(entry.first))  {
        os << "(" << entry.second << ")";
      }
    }
    os << "\n";
  }
}

void RequirementRepr::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}

void GenericParamList::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

void LayoutConstraint::dump() const {
  if (!*this) {
    llvm::errs() << "(null)\n";
    return;
  }
  getPointer()->print(llvm::errs());
}

void GenericSignature::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

void Requirement::dump() const {
  dump(llvm::errs());
  llvm::errs() << '\n';
}
void Requirement::dump(raw_ostream &out) const {
  DefaultWriter writer(out, /*indent*/ 0);
  PrintBase(writer).visitRequirement(*this);
}

void SILParameterInfo::dump() const {
  // TODO: Fix LifetimeDependenceInfo printing here.
  print(llvm::errs());
  llvm::errs() << '\n';
}

void SILResultInfo::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}
