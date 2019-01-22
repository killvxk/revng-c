/// \file Restructure.cpp
/// \brief FunctionPass that applies the comb to the RegionCFG of a function

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// Standard includes
#include <sstream>
#include <stdlib.h>

// LLVM includes
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/GenericDomTreeConstruction.h"
#include "llvm/Support/raw_os_ostream.h"

// revng includes
#include "revng/Support/Debug.h"
#include "revng/Support/IRHelpers.h"

// Local libraries includes
#include "revng-c/RestructureCFGPass/RegionCFGTree.h"
#include "revng-c/RestructureCFGPass/RestructureCFG.h"
#include "revng-c/RestructureCFGPass/Utils.h"

// Local includes
#include "MetaRegion.h"
#include "Flattening.h"

using namespace llvm;

using std::make_pair;
using std::pair;
using std::string;
using std::to_string;

// TODO: Move the initialization of the logger here from "Utils.h"
// Debug logger.
Logger<> CombLogger("restructure");

// EdgeDescriptor is a handy way to create and manipulate edges on the RegionCFG.
using EdgeDescriptor = std::pair<BasicBlockNode *, BasicBlockNode *>;

#if 0
static bool existsPath(BasicBlockNode &Source, BasicBlockNode &Target) {
  std::set<BasicBlockNode *> Visited;
  std::vector<BasicBlockNode *> Stack;

  Stack.push_back(&Source);
  while (!Stack.empty()) {
    BasicBlockNode *Vertex = Stack.back();
    Stack.pop_back();

    if (Vertex == &Target) {
      return true;
    }

    if (Visited.count(Vertex) == 0) {
      Visited.insert(Vertex);
      for (BasicBlockNode *Successor : Vertex->successors()) {
        Stack.push_back(Successor);
      }
    }
  }

  return false;
}
#endif

#if 0
static std::set<BasicBlockNode *> findReachableNodes2(RegionCFG &RegionCFG,
                                               ReachabilityPass &Reachability,
                                               BasicBlockNode &Source,
                                               BasicBlockNode &Target) {

  std::set<BasicBlock *> &ReachableBlocks =
      Reachability.reachableFrom(Source.basicBlock());
  std::set<BasicBlockNode *> ReachableNodes;
  BasicBlock *TargetBlock = Target.basicBlock();
  for (BasicBlock *Block : ReachableBlocks) {
    if (Reachability.existsPath(Block, TargetBlock)) {
      ReachableNodes.insert(&RegionCFG.get(Block));
    }
  }
  return ReachableNodes;
}
#endif

static std::set<EdgeDescriptor> getBackedges(RegionCFG &Graph) {

  // Some helper data structures.
  int Time = 0;
  std::map<BasicBlockNode *, int> StartTime;
  std::map<BasicBlockNode *, int> FinishTime;
  std::vector<std::pair<BasicBlockNode *, size_t>> Stack;

  // Set of backedges.
  std::set<EdgeDescriptor> Backedges;

  // Push the entry node in the exploration stack.
  BasicBlockNode &EntryNode = Graph.getEntryNode();
  Stack.push_back(make_pair(&EntryNode, 0));

  // Go through the exploration stack.
  while (!Stack.empty()) {
    auto StackElem = Stack.back();
    Stack.pop_back();
    BasicBlockNode *Vertex = StackElem.first;
    Time++;

    // Check if we are inspecting a vertex for the first time, and in case mark
    // the start time of the visit.
    if (StartTime.count(Vertex) == 0) {
      StartTime[Vertex] = Time;
    }

    // Successor exploraition
    size_t Index = StackElem.second;

    // If we are still successors to explore.
    if (Index < StackElem.first->successor_size()) {
      BasicBlockNode *Successor = Vertex->getSuccessorI(Index);
      Index++;
      Stack.push_back(make_pair(Vertex, Index));

      // We are in presence of a backedge.
      if (StartTime.count(Successor) != 0
          and FinishTime.count(Successor) == 0) {
        Backedges.insert(make_pair(Vertex, Successor));
      }

      // Enqueue the successor for the visit.
      if (StartTime.count(Successor) == 0) {
        Stack.push_back(make_pair(Successor, 0));
      }
    } else {

      // Mark the finish of the visit of a vertex.
      FinishTime[Vertex] = Time;
    }
  }

  return Backedges;
}

static bool mergeSCSStep(std::vector<MetaRegion> &MetaRegions) {
  for (auto RegionIt1 = MetaRegions.begin(); RegionIt1 != MetaRegions.end();
       RegionIt1++) {
    for (auto RegionIt2 = std::next(RegionIt1); RegionIt2 != MetaRegions.end();
         RegionIt2++) {
      bool Intersects = (*RegionIt1).intersectsWith(*RegionIt2);
      bool IsIncluded = (*RegionIt1).isSubSet(*RegionIt2);
      bool IsIncludedReverse = (*RegionIt2).isSubSet(*RegionIt1);
      bool AreEquivalent = (*RegionIt1).nodesEquality(*RegionIt2);
      if (Intersects and
          (((!IsIncluded) and (!IsIncludedReverse)) or AreEquivalent)) {
        (*RegionIt1).mergeWith(*RegionIt2);
        MetaRegions.erase(RegionIt2);
        return true;
      }
    }
  }

  return false;
}

static void simplifySCS(std::vector<MetaRegion> &MetaRegions) {
  bool Changes = true;
  while (Changes) {
    Changes = mergeSCSStep(MetaRegions);
  }
}

static void sortMetaRegions(std::vector<MetaRegion> &MetaRegions) {
  std::sort(MetaRegions.begin(),
            MetaRegions.end(),
            [](MetaRegion &First,
               MetaRegion &Second)
            { return First.getNodes().size() < Second.getNodes().size(); });
}

static void computeParents(std::vector<MetaRegion> &MetaRegions,
                    MetaRegion *RootMetaRegion) {
  for (MetaRegion &MetaRegion1 : MetaRegions) {
    bool ParentFound = false;
    for (MetaRegion &MetaRegion2 : MetaRegions) {
      if (&MetaRegion1 != &MetaRegion2) {
        if (MetaRegion1.isSubSet(MetaRegion2)) {

          if (CombLogger.isEnabled()) {
            CombLogger << "For metaregion: " << &MetaRegion1 << "\n";
            CombLogger << "parent found\n";
            CombLogger << &MetaRegion2 << "\n";
          }

          MetaRegion1.setParent(&MetaRegion2);
          ParentFound = true;
          break;
        }
      }
    }

    if (!ParentFound) {

      if (CombLogger.isEnabled()) {
        CombLogger << "For metaregion: " << &MetaRegion1 << "\n";
        CombLogger << "no parent found\n";
      }

      MetaRegion1.setParent(RootMetaRegion);
    }
  }
}

static std::vector<MetaRegion *> applyPartialOrder(std::vector<MetaRegion> &V) {
  std::vector<MetaRegion *> OrderedVector;
  std::set<MetaRegion *> Processed;

  while (V.size() != Processed.size()) {
    for (auto RegionIt1 = V.begin(); RegionIt1 != V.end();
         RegionIt1++) {
      if (Processed.count(&*RegionIt1) == 0) {
        bool FoundParent = false;
        for (auto RegionIt2 = V.begin(); RegionIt2 != V.end();
             RegionIt2++) {
          if ((RegionIt1 != RegionIt2) and Processed.count(&*RegionIt2) == 0) {
            if ((*RegionIt1).getParent() == &*RegionIt2) {
              FoundParent = true;
              break;
            }
          }
        }

        if (FoundParent == false) {
          OrderedVector.push_back(&*RegionIt1);
          Processed.insert(&*RegionIt1);
          break;
        }
      }
    }
  }

  std::reverse(OrderedVector.begin(), OrderedVector.end());
  return OrderedVector;
}

static bool alreadyInMetaregion(std::vector<MetaRegion> &V, BasicBlockNode *N) {

  // Scan all the metaregions and check if a node is already contained in one of
  // them
  for (MetaRegion &Region : V) {
    if (Region.containsNode(N)) {
      return true;
    }
  }

  return false;
}

static void removeNotReachables(RegionCFG &Graph) {

  // Remove nodes that have no predecessors (nodes that are the result of node
  // cloning and that remains dandling around).
  bool Difference = true;
  while (Difference) {
    Difference = false;
    BasicBlockNode *EntryNode = &Graph.getEntryNode();
    for (auto It = Graph.begin(); It != Graph.end(); It++) {
      if ((EntryNode != *It and (*It)->predecessor_size() == 0)) {
        Graph.removeNode(*It);
        Difference = true;
        break;
      }
    }
  }
}

static std::vector<MetaRegion>
createMetaRegions(const std::set<EdgeDescriptor> &Backedges) {
  std::vector<std::set<BasicBlockNode *>> Regions;
  for (auto &Backedge : Backedges) {
    auto SCSNodes = findReachableNodes(*Backedge.second, *Backedge.first);

    if (CombLogger.isEnabled()) {
      CombLogger << "SCS identified by: ";
      CombLogger << Backedge.first->getNameStr() << " -> "
          << Backedge.second->getNameStr() << "\n";
      CombLogger << "Is composed of nodes:\n";
      for (auto Node : SCSNodes) {
        CombLogger << Node->getNameStr() << "\n";
      }
    }

    Regions.push_back(SCSNodes);
  }

  for (auto RegionIt1 = Regions.begin(); RegionIt1 != Regions.end();
       RegionIt1++) {
    for (auto RegionIt2 = std::next(RegionIt1); RegionIt2 != Regions.end();
         RegionIt2++) {
      if (RegionIt1 != RegionIt2) {
        std::vector<BasicBlockNode *> Intersection;
        bool IsSubset = std::includes((*RegionIt1).begin(),
                                      (*RegionIt1).end(),
                                      (*RegionIt2).begin(),
                                      (*RegionIt2).end());
        std::set_intersection((*RegionIt1).begin(),
                              (*RegionIt1).end(),
                              (*RegionIt2).begin(),
                              (*RegionIt2).end(),
                              std::back_inserter(Intersection));

        if (CombLogger.isEnabled()) {
          CombLogger << "IsSubset: " << IsSubset << "\n";
          CombLogger << "Intersection between:\n";
          CombLogger << "1:\n";
          for (auto &Node : *RegionIt1) {
            CombLogger << Node->getNameStr() << "\n";
          }
          CombLogger << "2:\n";
          for (auto &Node : *RegionIt2) {
            CombLogger << Node->getNameStr() << "\n";
          }
          CombLogger << "is:\n";
          for (auto &Node : Intersection) {
            CombLogger << Node->getNameStr() << "\n";
          }
        }
      }
    }
  }

  std::vector<MetaRegion> MetaRegions;
  int SCSIndex = 1;
  for (size_t I = 0; I < Regions.size(); ++I) {
    auto &SCS = Regions[I];
    MetaRegions.push_back(MetaRegion(SCSIndex, SCS, true));
    SCSIndex++;
  }
  return MetaRegions;
}

char RestructureCFG::ID = 0;
static RegisterPass<RestructureCFG> X("restructureCFG",
                                      "Apply RegionCFG restructuring transformation",
                                      true,
                                      true);

bool RestructureCFG::runOnFunction(Function &F) {

  // Analyze only isolated functions.
  if (!F.getName().startswith("bb.")) {
    return false;
  }

  // Clear graph object from the previous pass.
  CompleteGraph = RegionCFG();

  // Set names of the CFG region
  CompleteGraph.setFunctionName(F.getName());
  CompleteGraph.setRegionName("root");

  // Logger object
  auto &Log = CombLogger;

  // Random seed initialization
  srand(time(NULL));

  // Initialize the RegionCFG object
  CompleteGraph.initialize(F);
  RegionCFG &Graph = CompleteGraph;

  // Dump the object in .dot format if debug mode is activated.
  if (Log.isEnabled()) {
    CompleteGraph.dumpDotOnFile("dots", F.getName(), "begin");
  }

  // Identify SCS regions.
  if (CombLogger.isEnabled()) {
    BasicBlockNode &FirstRandom = CompleteGraph.getRandomNode();
    BasicBlockNode &SecondRandom = CompleteGraph.getRandomNode();
    Log << "Source: ";
    Log << FirstRandom.getNameStr() << "\n";
    Log << "Target: ";
    Log << SecondRandom.getNameStr() << "\n";
    Log << "Nodes Reachable:\n";
    std::set<BasicBlockNode *> Reachables = findReachableNodes(FirstRandom,
                                                               SecondRandom);
    for (BasicBlockNode *Element : Reachables) {
      Log << Element->getNameStr() << "\n";
    }
  }

  std::set<EdgeDescriptor> Backedges = getBackedges(CompleteGraph);
  Log << "Backedges in the graph:\n";
  for (auto &Backedge : Backedges) {
    Log << Backedge.first->getNameStr() << " -> "
        << Backedge.second->getNameStr() << "\n";
  }

  // Create meta regions
  std::vector<MetaRegion> MetaRegions = createMetaRegions(Backedges);

  // Simplify SCS in a fixed-point fashion.
  simplifySCS(MetaRegions);

  // Print SCS after simplification.
  if (Log.isEnabled()) {
    Log << "\n";
    Log << "Metaregions after simplification:\n";
    for (auto &Meta : MetaRegions) {
      Log << "\n";
      Log << &Meta << "\n";
      auto &Nodes = Meta.getNodes();
      Log << "Is composed of nodes:\n";
      for (auto *Node : Nodes) {
        Log << Node->getNameStr() << "\n";
      }
    }
  }

  // Sort the Metaregions in increasing number of composing nodes order.
  sortMetaRegions(MetaRegions);

  // Print SCS after ordering.
  if (Log.isEnabled()) {
    Log << "\n";
    Log << "Metaregions after ordering:\n";
    for (auto &Meta : MetaRegions) {
      Log << "\n";
      Log << &Meta << "\n";
      Log << "Is composed of nodes:\n";
      auto &Nodes = Meta.getNodes();
      for (auto *Node : Nodes) {
        Log << Node->getNameStr() << "\n";
      }
    }
  }

  // Compute parent relations for the identified SCSs.
  std::set<BasicBlockNode *> Empty;
  MetaRegion RootMetaRegion(0, Empty);
  computeParents(MetaRegions, &RootMetaRegion);

  // Print metaregions after ordering.
  if (Log.isEnabled()) {
    Log << "\n";
    Log << "Metaregions parent relationship:\n";
    for (auto &Meta : MetaRegions) {
      Log << "\n";
      Log << &Meta << "\n";
      auto &Nodes = Meta.getNodes();
      Log << "Is composed of nodes:\n";
      for (auto *Node : Nodes) {
        Log << Node->getNameStr() << "\n";
      }
      Log << "Has parent: " << Meta.getParent() << "\n";
    }
  }

  // Find an ordering for the metaregions that satisfies the inclusion
  // relationship. We create a new "shadow" vector containing only pointers to
  // the "real" metaregions.
  std::vector<MetaRegion *> OrderedMetaRegions = applyPartialOrder(MetaRegions);

  // Print metaregions after ordering.
  if (Log.isEnabled()) {
    Log << "\n";
    Log << "Metaregions after ordering:\n";
    for (auto *Meta : OrderedMetaRegions) {
      Log << "\n";
      Log << Meta << "\n";
      Log << "With index " << Meta->getIndex() << "\n";
      Log << "With size " << Meta->nodes_size() << "\n";
      auto &Nodes = Meta->getNodes();
      Log << "Is composed of nodes:\n";
      for (auto *Node : Nodes) {
        Log << Node->getNameStr() << "\n";
      }
      Log << "Has parent: " << Meta->getParent() << "\n";
      Log << "Is SCS: " << Meta->isSCS() << "\n";
    }
  }

  ReversePostOrderTraversal<BasicBlockNode *> RPOT(&CompleteGraph.getEntryNode());
  if (Log.isEnabled()) {
    Log << "Reverse post order is:\n";
    for (BasicBlockNode *BN : RPOT) {
      Log << BN->getNameStr() << "\n";
    }
    Log << "Reverse post order end\n";
  }

  Log << "Debugged function" << "\n";
  Log << F.getName().equals("bb._start_c") << "\n";

  DominatorTreeBase<BasicBlockNode, false> DT;
  DT.recalculate(CompleteGraph);

  DominatorTreeBase<BasicBlockNode, true> PDT;
  PDT.recalculate(CompleteGraph);

  // Some debug information on dominator and postdominator tree.
  if (Log.isEnabled()) {
    Log << DT.isPostDominator() << "\n";
    Log << "The root node of the dominator tree is:\n";
    Log << DT.getRoot()->getNameStr() << "\n";
    Log << "Between these two nodes:\n";
    BasicBlockNode *Random = &CompleteGraph.getRandomNode();
    BasicBlockNode *Random2 = &CompleteGraph.getRandomNode();
    Log << Random->getNameStr() << "\n";
    Log << Random2->getNameStr() << "\n";
    Log << "Dominance:\n";
    Log << DT.dominates(Random, Random2) << "\n";
    Log << "PostDominance:\n";
    Log << PDT.dominates(Random, Random2) << "\n";
    Log << PDT.isPostDominator() << "\n";
  }


  std::vector<RegionCFG> Regions(OrderedMetaRegions.size());
  for (MetaRegion *Meta : OrderedMetaRegions) {
    if (Log.isEnabled()) {
      Log << "\nAnalyzing region: " << Meta->getIndex() <<"\n";
    }

    // Refresh backedges, since some of them may have been modified during
    // the transformations
    Backedges = getBackedges(CompleteGraph);

    if (Log.isEnabled()) {

      auto &Nodes = Meta->getNodes();
      Log << "Which is composed of nodes:\n";
      for (auto *Node : Nodes) {
        Log << Node->getNameStr() << "\n";
      }

      Log << "Dumping main graph snapshot before restructuring\n";
      Graph.dumpDotOnFile("dots",
                          F.getName(),
                          "Out-pre-" + std::to_string(Meta->getIndex()));
    }

    std::map<BasicBlockNode *, int> IncomingDegree;
    for (BasicBlockNode *Node : Meta->nodes()) {
      int IncomingCounter = 0;
      for (BasicBlockNode *Predecessor : Node->predecessors()) {
        EdgeDescriptor Edge = make_pair(Predecessor, Node);
        if ((Meta->containsNode(Predecessor))
            and (Backedges.count(Edge))) {
          IncomingCounter++;
        }
      }
      IncomingDegree[Node] = IncomingCounter;
    }

    // Print information about incoming edge degrees.
    if (Log.isEnabled()) {
      Log << "Incoming degree:\n";
      for (auto &it : IncomingDegree) {
        Log << it.first->getNameStr() << " " << it.second << "\n";
      }
    }

    auto MaxDegreeIt = max_element(IncomingDegree.begin(),
                                   IncomingDegree.end(),
                                   [](const pair<BasicBlockNode *, int> &p1,
                                      const pair<BasicBlockNode *, int> &p2)
                                   { return p1.second < p2.second; });
    int MaxDegree = (*MaxDegreeIt).second;

    if (Log.isEnabled()) {
      Log << "Maximum incoming degree found: ";
      Log << MaxDegree << "\n";
    }

    std::set<BasicBlockNode *> MaximuxEdgesNodes;
    copy_if(Meta->begin(),
            Meta->end(),
            std::inserter(MaximuxEdgesNodes, MaximuxEdgesNodes.begin()),
            [&IncomingDegree, &MaxDegree]
            (BasicBlockNode *Node)
            { return IncomingDegree[Node] == MaxDegree; });

    revng_assert(MaxDegree > 0);

    BasicBlockNode *FirstCandidate;
    if (MaximuxEdgesNodes.size() > 1) {
      for (BasicBlockNode *BN : RPOT) {
        if (MaximuxEdgesNodes.count(BN) != 0) {
          FirstCandidate = BN;
          break;
        }
      }
    } else {
      FirstCandidate = *MaximuxEdgesNodes.begin();
    }

    // Print out the name of the node that has been selected as head of the
    // region
    if (Log.isEnabled()) {
      Log << "Elected head is: " << FirstCandidate->getNameStr() << "\n";
    }

    // Identify all the abnormal retreating edges in a SCS.
    std::set<EdgeDescriptor> Retreatings;
    std::set<BasicBlockNode *> RetreatingTargets;
    for (EdgeDescriptor Backedge : Backedges) {
      if (Meta->containsNode(Backedge.first)) {
        Retreatings.insert(Backedge);
        RetreatingTargets.insert(Backedge.second);
      }
    }
    if (Log.isEnabled()) {
      Log << "Retreatings found:\n";
      for (EdgeDescriptor Retreating : Retreatings) {
        Log << Retreating.first->getNameStr() << " -> ";
        Log << Retreating.second->getNameStr() << "\n";
      }
    }

    bool NewHeadNeeded = false;
    for (BasicBlockNode *Node : RetreatingTargets) {
      if (Node != FirstCandidate) {
        NewHeadNeeded = true;
      }
    }
    if (Log.isEnabled()) {
      Log << "New head needed: " << NewHeadNeeded << "\n";
    }

    BasicBlockNode *Head;
    if (NewHeadNeeded) {
      Head = CompleteGraph.addDummyNode("head dispatcher");
      Meta->insertNode(Head);

      // Move the incoming edge from the old head to new one.
      for (BasicBlockNode *Predecessor : FirstCandidate->predecessors()) {
        if (!Meta->containsNode(Predecessor)) {
          moveEdgeTarget(EdgeDescriptor(Predecessor, FirstCandidate), Head);
        }
      }

      // Build the tree dispatcher structure.
      BasicBlockNode *Dummy = Head;
      std::map<BasicBlockNode *, int> RetreatingIdxMap;
      int Idx = 0;
      for (BasicBlockNode *Target : RetreatingTargets) {
        BasicBlockNode *NewDummy = CompleteGraph.addDummyNode("entry dummy dispatcher "
                                                      "idx " + to_string(Idx));
        Meta->insertNode(NewDummy);
        RetreatingIdxMap[Target] = Idx;
        Idx++;
        addEdge(EdgeDescriptor(Dummy, Target));
        addEdge(EdgeDescriptor(Dummy, NewDummy));
        Dummy = NewDummy;
      }
      for (EdgeDescriptor Retreating : Retreatings) {
        Idx = RetreatingIdxMap[Retreating.second];
        string NodeName = "entry idx set " + std::to_string(Idx);
        BasicBlockNode *IdxSetNode = CompleteGraph.addDummyNode(NodeName);
        Meta->insertNode(IdxSetNode);
        addEdge(EdgeDescriptor(Retreating.first, IdxSetNode));
        addEdge(EdgeDescriptor(IdxSetNode, Head));
        removeEdge(EdgeDescriptor(Retreating.first, Retreating.second));
      }
    } else {
      Head = FirstCandidate;
    }

    revng_assert(Head != nullptr);
    if (Log.isEnabled()) {
      Log << "New head name is: " << Head->getNameStr() << "\n";
    }

    // Successor refinement step.
    std::set<BasicBlockNode *> Successors = Meta->getSuccessors();

    if (Log.isEnabled()) {
      Log << "Region successors are:\n";
      for (BasicBlockNode *Node : Successors) {
        Log << Node->getNameStr() << "\n";
      }
    }

    bool AnotherIteration = true;
    while (AnotherIteration and Successors.size() > 1) {
      AnotherIteration = false;
      std::set<EdgeDescriptor> OutgoingEdges = Meta->getOutEdges();

      std::vector<BasicBlockNode *> Frontiers;
      std::map<BasicBlockNode *,
               pair<BasicBlockNode *, BasicBlockNode *>> EdgeExtremal;

      for (EdgeDescriptor Edge : OutgoingEdges) {
        BasicBlockNode *Frontier = CompleteGraph.addDummyNode("frontier");
        BasicBlockNode *OldSource = Edge.first;
        BasicBlockNode *OldTarget = Edge.second;
        EdgeExtremal[Frontier] = make_pair(OldSource, OldTarget);
        moveEdgeTarget(Edge, Frontier);
        addEdge(EdgeDescriptor(Frontier, OldTarget));
        Meta->insertNode(Frontier);
        Frontiers.push_back(Frontier);
      }

      DT.recalculate(CompleteGraph);
      for (BasicBlockNode *Frontier : Frontiers) {
        for (BasicBlockNode *Successor : Successors) {
          if ((DT.dominates(Head, Successor))
              and (DT.dominates(Frontier, Successor))
              and !alreadyInMetaregion(MetaRegions, Successor)) {
            Meta->insertNode(Successor);
            AnotherIteration = true;
            if (Log.isEnabled()) {
              Log << "Identified new candidate for successor refinement:";
              Log << Successor->getNameStr() << "\n";
            }
          }
        }
      }

      // Remove the frontier nodes since we do not need them anymore.
      for (BasicBlockNode *Frontier : Frontiers) {
        BasicBlockNode *OriginalSource = EdgeExtremal[Frontier].first;
        BasicBlockNode *OriginalTarget = EdgeExtremal[Frontier].second;
        addEdge(EdgeDescriptor(OriginalSource, OriginalTarget));
        CompleteGraph.removeNode(Frontier);
        Meta->removeNode(Frontier);
      }

      Successors = Meta->getSuccessors();
    }

    // First Iteration outlining.
    // Clone all the nodes of the SCS except for the head.
    std::map<BasicBlockNode *, BasicBlockNode *> ClonedMap;
    for (BasicBlockNode *Node : Meta->nodes()) {
      if (Node != Head) {
        BasicBlockNode *Clone = CompleteGraph.cloneNode(*Node);
        ClonedMap[Node] = Clone;
      }
    }

    // Restore edges between cloned nodes.
    for (BasicBlockNode *Node : Meta->nodes()) {
      if (Node != Head) {

        // Handle outgoing edges from SCS nodes.
        for (BasicBlockNode *Successor : Node->successors()) {
          if (Meta->containsNode(Successor)) {
            // Handle edges pointing inside the SCS.
            if ((Successor == Head) or (Successor == FirstCandidate)) {
              // Retreating edges should point to the new head.
              addEdge(EdgeDescriptor(ClonedMap[Node], Head));
            } else {
              // Other edges should be restored between cloned nodes.
              addEdge(EdgeDescriptor(ClonedMap[Node], ClonedMap[Successor]));
            }
          } else {
            // Edges exiting from the SCS should go to the right target.
            addEdge(EdgeDescriptor(ClonedMap[Node], Successor));
          }
        }

        // Handle incoming edges in SCS nodes.
        for (BasicBlockNode *Predecessor : Node->predecessors()) {
          if (!Meta->containsNode(Predecessor)) {
            addEdge(EdgeDescriptor(Predecessor, ClonedMap[Node]));
            removeEdge(EdgeDescriptor(Predecessor, Node));
          }
        }
      }
    }

    // Exit dispatcher creation.
    // TODO: Factorize this out together with the head dispatcher creation.
    bool NewExitNeeded = false;
    BasicBlockNode *Exit;
    std::vector<BasicBlockNode *> ExitDispatcherNodes;
    if (Successors.size() > 1) {
      NewExitNeeded = true;
    }
    if (Log.isEnabled()) {
      Log << "New exit needed: " << NewExitNeeded << "\n";
    }

    if (NewExitNeeded) {
      Exit = CompleteGraph.addDummyNode("exit dispatcher");
      ExitDispatcherNodes.push_back(Exit);
      std::set<EdgeDescriptor> OutEdges = Meta->getOutEdges();

      // Build the tree dispatcher structure.
      BasicBlockNode *Dummy = Exit;
      std::map<BasicBlockNode *, int> SuccessorsIdxMap;
      int Idx = 0;
      for (BasicBlockNode *Target : Successors) {
        BasicBlockNode *NewDummy = CompleteGraph.addDummyNode("exit dummy dispatcher "
                                                      + to_string(Idx));
        ExitDispatcherNodes.push_back(NewDummy);
        SuccessorsIdxMap[Target] = Idx;
        Idx++;
        addEdge(EdgeDescriptor(Dummy, Target));
        addEdge(EdgeDescriptor(Dummy, NewDummy));
        Dummy = NewDummy;
      }
      for (EdgeDescriptor Edge : OutEdges) {
        Idx = SuccessorsIdxMap[Edge.second];
        string NodeName = "exit idx " + std::to_string(Idx);
        BasicBlockNode *IdxSetNode = CompleteGraph.addDummyNode(NodeName);
        Meta->insertNode(IdxSetNode);
        addEdge(EdgeDescriptor(Edge.first, IdxSetNode));
        addEdge(EdgeDescriptor(IdxSetNode, Edge.second));
        removeEdge(EdgeDescriptor(Edge.first, Edge.second));
      }
      if (Log.isEnabled()) {
        Log << "New exit name is: " << Exit->getNameStr() << "\n";
      }
    }

    // Collapse Region.
    // Create a new RegionCFG object for representing the collapsed region and
    // populate it with the internal nodes.
    std::set<EdgeDescriptor> OutgoingEdges = Meta->getOutEdges();
    std::set<EdgeDescriptor> IncomingEdges = Meta->getInEdges();
    RegionCFG CollapsedGraph{};
    RegionCFG::BBNodeMap SubstitutionMap{};
    CollapsedGraph.setFunctionName(F.getName());
    CollapsedGraph.setRegionName(std::to_string(Meta->getIndex()));
    revng_assert(Head != nullptr);
    CollapsedGraph.insertBulkNodes(Meta->getNodes(), Head, SubstitutionMap);

    // Create the break and continue node.
    BasicBlockNode *Continue = CollapsedGraph.addDummyContinue();
    BasicBlockNode *Break = CollapsedGraph.addDummyBreak();

    // Connect the break and continue nodes with the necessary edges.
    CollapsedGraph.connectContinueNode(Continue);
    CollapsedGraph.connectBreakNode(OutgoingEdges, Break, SubstitutionMap);

    // Create the collapsed node in the outer region.
    BasicBlockNode *CollapsedNode = CompleteGraph.createCollapsedNode(&CollapsedGraph);

    // Connect the old incoming edges to the collapsed node.
    for (EdgeDescriptor Edge : IncomingEdges)
      moveEdgeTarget(Edge, CollapsedNode);

    // Connect the outgoing edges to the collapsed node.
    if (NewExitNeeded) {
      revng_assert(Exit != nullptr);
      addEdge(EdgeDescriptor(CollapsedNode, Exit));
    } else {

      // Double check that we have at most a single successor
      revng_assert(Successors.size() <= 1);
      if (Successors.size() == 1) {

        //Connect the collapsed node to the unique successor
        BasicBlockNode *Successor = *Successors.begin();
        addEdge(EdgeDescriptor(CollapsedNode, Successor));
      }
    }

    // Remove collapsed nodes from the outer region.
    for (BasicBlockNode *Node : Meta->nodes()) {
      if (Log.isEnabled()) {
        Log << "Removing from main graph node :" << Node->getNameStr() << "\n";
      }
      CompleteGraph.removeNode(Node);
    }

    // Substitute in the other SCSs the nodes of the current SCS with the
    // collapsed node and the exit dispatcher structure.
    for (MetaRegion *OtherMeta : OrderedMetaRegions) {
      if (OtherMeta != Meta) {
        OtherMeta->updateNodes(Meta->getNodes(),
                               CollapsedNode,
                               ExitDispatcherNodes);
      }
    }

    // Replace the pointers inside SCS.
    Meta->replaceNodes(CollapsedGraph.getNodes());

    // Remove useless nodes inside the SCS (like dandling break/continue)
    removeNotReachables(CollapsedGraph);

    // Serialize the newly collapsed SCS region.
    if (Log.isEnabled()) {
      Log << "Dumping CFG of metaregion " << Meta->getIndex() << "\n";
      CollapsedGraph.dumpDotOnFile("dots",
                                   F.getName(),
                                   "In-" + std::to_string(Meta->getIndex()));
      Log << "Dumping main graph snapshot post restructuring\n";
      Graph.dumpDotOnFile("dots",
                          F.getName(),
                          "Out-post-" + std::to_string(Meta->getIndex()));
    }
    Regions.push_back(std::move(CollapsedGraph));
  }

  // Serialize the newly collapsed SCS region.
  if (Log.isEnabled()) {
    Log << "Dumping main graph before final purge\n";
    Graph.dumpDotOnFile("dots", F.getName(), "Final-before-purge");
  }

  // Remove not reachables nodes from the main final graph.
  removeNotReachables(CompleteGraph);

  // Serialize the newly collapsed SCS region.
  if (Log.isEnabled()) {
    Log << "Dumping main graph after final purge\n";
    Graph.dumpDotOnFile("dots", F.getName(), "Final-after-purge");
  }

  // Print metaregions after ordering.
  if (Log.isEnabled()) {
    Log << "\n";
    Log << "Metaregions after collapse:\n";
    for (auto *Meta : OrderedMetaRegions) {
      Log << "\n";
      Log << Meta << "\n";
      Log << "With index " << Meta->getIndex() << "\n";
      Log << "With size " << Meta->nodes_size() << "\n";
      auto &Nodes = Meta->getNodes();
      Log << "Is composed of nodes:\n";
      for (auto *Node : Nodes) {
        Log << Node->getNameStr() << "\n";
      }
      Log << "Has parent: " << Meta->getParent() << "\n";
      Log << "Is SCS: " << Meta->isSCS() << "\n";
    }
  }

  // Invoke the AST generation for the root region.
  Log.emit();
  ASTNode *RootNode = CompleteGraph.generateAst();

  // Serialize AST on a file named as the function
  {
    std::ofstream ASTFile;
    ASTFile.open("ast/" + F.getName().str() + ".dot");
    ASTFile << "digraph CFGFunction {\n";
    RootNode->dump(ASTFile);
    ASTFile << "}\n";
    ASTFile.close();
  }

  // Serialize AST on stderr
  if (Log.isEnabled()) {
    Log << "\nFinal AST is:\n";
    Log << "digraph CFGFunction {\n";
    dumpNode(RootNode);
    Log << "}\n";
  }

  // Sync Logger.
  Log.emit();

  // Early exit if the AST generation produced a version of the AST which is
  // identical to the cached version.
  // In that case there's no need to flatten the RegionCFG.
  // TODO: figure out how to decide when we're done
  if (Done)
    return false;

  flattenRegionCFGTree(CompleteGraph, Regions);

  // Serialize the newly collapsed SCS region.
  if (Log.isEnabled()) {
    Log << "Dumping main graph after Flattening\n";
    CompleteGraph.dumpDotOnFile("dots", F.getName(), "final-after-flattening");
  }

  return false;
}
