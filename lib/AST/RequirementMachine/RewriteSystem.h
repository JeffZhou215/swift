//===--- RewriteSystem.h - Generics with term rewriting ---------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_REWRITESYSTEM_H
#define SWIFT_REWRITESYSTEM_H

#include "llvm/ADT/DenseSet.h"

#include "Debug.h"
#include "ProtocolGraph.h"
#include "Symbol.h"
#include "Term.h"
#include "Trie.h"

namespace llvm {
  class raw_ostream;
}

namespace swift {

namespace rewriting {

class PropertyMap;
class RewriteContext;
class RewriteSystem;

/// A rewrite rule that replaces occurrences of LHS with RHS.
///
/// LHS must be greater than RHS in the linear order over terms.
///
/// Out-of-line methods are documented in RewriteSystem.cpp.
class Rule final {
  Term LHS;
  Term RHS;
  bool deleted;

public:
  Rule(Term lhs, Term rhs)
      : LHS(lhs), RHS(rhs), deleted(false) {}

  const Term &getLHS() const { return LHS; }
  const Term &getRHS() const { return RHS; }

  /// Returns if the rule was deleted.
  bool isDeleted() const {
    return deleted;
  }

  /// Deletes the rule, which removes it from consideration in term
  /// simplification and completion. Deleted rules are simply marked as
  /// such instead of being physically removed from the rules vector
  /// in the rewrite system, to ensure that indices remain valid across
  /// deletion.
  void markDeleted() {
    assert(!deleted);
    deleted = true;
  }

  /// Returns the length of the left hand side.
  unsigned getDepth() const {
    return LHS.size();
  }

  void dump(llvm::raw_ostream &out) const;

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                       const Rule &rule) {
    rule.dump(out);
    return out;
  }
};

struct AppliedRewriteStep {
  Term lhs;
  Term rhs;
  MutableTerm prefix;
  MutableTerm suffix;
};

/// Records the application of a rewrite rule to a term.
///
/// Formally, this is a whiskered, oriented rewrite rule. For example, given a
/// rule (X => Y) and the term A.X.B, the application at offset 1 yields A.Y.B.
///
/// This can be represented as A.(X => Y).B.
///
/// Similarly, going in the other direction, if we start from A.Y.B and apply
/// the inverse rule, we get A.(Y => X).B.
struct RewriteStep {
  enum StepKind {
    /// Apply a rewrite rule at the stored offset.
    ApplyRewriteRule,

    /// Prepend the prefix to each concrete substitution.
    AdjustConcreteType
  };

  /// The rewrite step kind.
  unsigned Kind : 1;

  /// The position within the term where the rule is being applied.
  unsigned Offset : 15;

  /// The index of the rule in the rewrite system.
  unsigned RuleID : 15;

  /// If false, the step replaces an occurrence of the rule's left hand side
  /// with the right hand side. If true, vice versa.
  unsigned Inverse : 1;

  RewriteStep(StepKind kind, unsigned offset, unsigned ruleID, bool inverse) {
    Kind = unsigned(kind);

    Offset = offset;
    assert(Offset == offset && "Overflow");
    RuleID = ruleID;
    assert(RuleID == ruleID && "Overflow");
    Inverse = inverse;
  }

  static RewriteStep forRewriteRule(unsigned offset, unsigned ruleID, bool inverse) {
    return RewriteStep(ApplyRewriteRule, offset, ruleID, inverse);
  }

  static RewriteStep forAdjustment(unsigned offset, bool inverse) {
    return RewriteStep(AdjustConcreteType, offset, /*ruleID=*/0, inverse);
  }

  void invert() {
    Inverse = !Inverse;
  }

  AppliedRewriteStep applyRewriteRule(MutableTerm &term,
                                      const RewriteSystem &system) const;

  MutableTerm applyAdjustment(MutableTerm &term,
                              const RewriteSystem &system) const;

  void dump(llvm::raw_ostream &out,
            MutableTerm &term,
            const RewriteSystem &system) const;
};

/// Records a sequence of zero or more rewrite rules applied to a term.
struct RewritePath {
  SmallVector<RewriteStep, 3> Steps;

  bool empty() const {
    return Steps.empty();
  }

  void add(RewriteStep step) {
    Steps.push_back(step);
  }

  // Horizontal composition of paths.
  void append(RewritePath other) {
    Steps.append(other.begin(), other.end());
  }

  decltype(Steps)::const_iterator begin() const {
    return Steps.begin();
  }

  decltype(Steps)::const_iterator end() const {
    return Steps.end();
  }

  void invert();

  void dump(llvm::raw_ostream &out,
            MutableTerm term,
            const RewriteSystem &system) const;
};

/// A term rewrite system for working with types in a generic signature.
///
/// Out-of-line methods are documented in RewriteSystem.cpp.
class RewriteSystem final {
  /// Rewrite context for memory allocation.
  RewriteContext &Context;

  /// The rules added so far, including rules from our client, as well
  /// as rules introduced by the completion procedure.
  std::vector<Rule> Rules;

  /// A prefix trie of rule left hand sides to optimize lookup. The value
  /// type is an index into the Rules array defined above.
  Trie<unsigned, MatchKind::Shortest> Trie;

  /// The graph of all protocols transitively referenced via our set of
  /// rewrite rules, used for the linear order on symbols.
  ProtocolGraph Protos;

  /// Constructed from a rule of the form X.[P2:T] => X.[P1:T] by
  /// checkMergedAssociatedType().
  struct MergedAssociatedType {
    /// The *right* hand side of the original rule, X.[P1:T].
    Term rhs;

    /// The associated type symbol appearing at the end of the *left*
    /// hand side of the original rule, [P2:T].
    Symbol lhsSymbol;

    /// The merged associated type symbol, [P1&P2:T].
    Symbol mergedSymbol;
  };

  /// A list of pending terms for the associated type merging completion
  /// heuristic. Entries are added by checkMergedAssociatedType(), and
  /// consumed in processMergedAssociatedTypes().
  std::vector<MergedAssociatedType> MergedAssociatedTypes;

  /// Pairs of rules which have already been checked for overlap.
  llvm::DenseSet<std::pair<unsigned, unsigned>> CheckedOverlaps;

  /// Homotopy generators (2-cells) for this rewrite system. These are the
  /// cyclic rewrite paths which rewrite a term back to itself. This
  /// data informs the generic signature minimization algorithm.
  std::vector<std::pair<MutableTerm, RewritePath>> HomotopyGenerators;

  DebugOptions Debug;

public:
  explicit RewriteSystem(RewriteContext &ctx);
  ~RewriteSystem();

  RewriteSystem(const RewriteSystem &) = delete;
  RewriteSystem(RewriteSystem &&) = delete;
  RewriteSystem &operator=(const RewriteSystem &) = delete;
  RewriteSystem &operator=(RewriteSystem &&) = delete;

  /// Return the rewrite context used for allocating memory.
  RewriteContext &getRewriteContext() const { return Context; }

  /// Return the object recording information about known protocols.
  const ProtocolGraph &getProtocols() const { return Protos; }

  void initialize(std::vector<std::pair<MutableTerm, MutableTerm>> &&rules,
                  ProtocolGraph &&protos);

  Symbol simplifySubstitutionsInSuperclassOrConcreteSymbol(Symbol symbol) const;

  unsigned getRuleID(const Rule &rule) const {
    assert((unsigned)(&rule - &*Rules.begin()) < Rules.size());
    return (unsigned)(&rule - &*Rules.begin());
  }

  Rule &getRule(unsigned ruleID) {
    return Rules[ruleID];
  }

  const Rule &getRule(unsigned ruleID) const {
    return Rules[ruleID];
  }

  bool addRule(MutableTerm lhs, MutableTerm rhs,
               const RewritePath *path=nullptr);

  bool simplify(MutableTerm &term, RewritePath *path=nullptr) const;

  enum class CompletionResult {
    /// Confluent completion was computed successfully.
    Success,

    /// Maximum number of iterations reached.
    MaxIterations,

    /// Completion produced a rewrite rule whose left hand side has a length
    /// exceeding the limit.
    MaxDepth
  };

  std::pair<CompletionResult, unsigned>
  computeConfluentCompletion(unsigned maxIterations,
                             unsigned maxDepth);

  void simplifyRewriteSystem();

  void verifyRewriteRules() const;

  void verifyHomotopyGenerators() const;

  std::pair<CompletionResult, unsigned>
  buildPropertyMap(PropertyMap &map,
                   unsigned maxIterations,
                   unsigned maxDepth);

  void dump(llvm::raw_ostream &out) const;

private:
  bool
  computeCriticalPair(
      ArrayRef<Symbol>::const_iterator from,
      const Rule &lhs, const Rule &rhs,
      std::vector<std::pair<MutableTerm, MutableTerm>> &pairs,
      std::vector<RewritePath> &paths,
      std::vector<std::pair<MutableTerm, RewritePath>> &loops) const;

  void processMergedAssociatedTypes();

  void checkMergedAssociatedType(Term lhs, Term rhs);
};

} // end namespace rewriting

} // end namespace swift

#endif
