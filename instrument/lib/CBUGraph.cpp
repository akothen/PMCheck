
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/PassManager.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Pass.h"

#include "DataStructure.h"
#include "DSGraph.h"

using namespace llvm;


char BUDataStructures::ID = 0;
char CompleteBUDataStructures::ID = 0;

static RegisterPass<CompleteBUDataStructures> X("cbudatastructure",
                                                    "Complete Bottom-up Data Structure Analysis");

//Statistic<> NumCBUInlines("cbudatastructures", "Number of graphs inlined");

//INITIALIZE_PASS_BEGIN(CompleteBUDataStructures,
	// "redundant-persist instructions-check",
  //        "Perform Check on Insts", true, true)
//INITIALIZE_PASS_END(CompleteBUDataStructures,
	// "redundant-persist instructions-check",
    //     "Perform Check on Insts", true, true)

bool CompleteBUDataStructures::runOnModule(Module &M) {
  BUDataStructures &BU = getAnalysis<BUDataStructures>();
  GlobalECs = BU.getGlobalECs();
  GlobalsGraph = new DSGraph(BU.getGlobalsGraph(), GlobalECs);
  GlobalsGraph->setPrintAuxCalls();

  // Our call graph is the same as the BU data structures call graph
  ActualCallees = BU.getActualCallees();
  std::vector<DSGraph *> Stack;
  std::unordered_map<DSGraph *, unsigned> ValMap;
  unsigned NextID = 1;
  Function *MainFunc = nullptr;
  for(auto &F : M) {
    MainFunc = F.getName() == "main" ? &F : nullptr;
    if(MainFunc) {
      if(MainFunc->size())
        calculateSCCGraphs(getOrCreateGraph(*MainFunc), Stack, NextID, ValMap);
    } //else {
      //std::cerr << "CBU-DSA: No 'main' function found!\n";
    //}
  }

  for(auto &F : M) {
    if(F.size() && !DSInfo.count(&F))
      calculateSCCGraphs(getOrCreateGraph(F), Stack, NextID, ValMap);
  }
  GlobalsGraph->removeTriviallyDeadNodes();

  // Merge the globals variables (not the calls) from the globals graph back
  // into the main function's graph so that the main function contains all of
  // the information about global pools and GV usage in the program.
  if (MainFunc && MainFunc->size()) {
    DSGraph &MainGraph = getOrCreateGraph(*MainFunc);
    const DSGraph &GG = *MainGraph.getGlobalsGraph();
    ReachabilityCloner RC(MainGraph, GG,
                          DSGraph::DontCloneCallNodes |
                          DSGraph::DontCloneAuxCallNodes);
    // Clone the global nodes into this graph.
    for (DSScalarMap::global_iterator I = GG.getScalarMap().global_begin(),
           E = GG.getScalarMap().global_end(); I != E; ++I)
      if (isa<GlobalVariable>(*I))
        RC.getClonedNH(GG.getNodeForValue(*I));
    MainGraph.maskIncompleteMarkers();
    MainGraph.markIncompleteNodes(DSGraph::MarkFormalArgs |
                                  DSGraph::IgnoreGlobals);
  }
  return false;
}

DSGraph &CompleteBUDataStructures::getOrCreateGraph(Function &F) {
  // Has the graph already been created?
  DSGraph *&Graph = DSInfo[&F];
  if (Graph) return *Graph;

  // Copy the BU graph...
  Graph = new DSGraph(getAnalysis<BUDataStructures>().getDSGraph(F), GlobalECs);
  Graph->setGlobalsGraph(GlobalsGraph);
  Graph->setPrintAuxCalls();

  // Make sure to update the DSInfo map for all of the functions currently in
  // this graph!
  for (DSGraph::retnodes_iterator I = Graph->retnodes_begin();
       I != Graph->retnodes_end(); ++I) {
    DSInfo[I->first] = Graph;
  }
  return *Graph;
}

unsigned CompleteBUDataStructures::calculateSCCGraphs(DSGraph &FG,
                                                  std::vector<DSGraph*> &Stack,
                                                  unsigned &NextID,
                                         std::unordered_map<DSGraph*, unsigned> &ValMap) {
  assert(!ValMap.count(&FG) && "Shouldn't revisit functions!");
  unsigned Min = NextID++, MyID = Min;
  ValMap[&FG] = Min;
  Stack.push_back(&FG);

  // The edges out of the current node are the call site targets...
  for (DSGraph::fc_iterator CI = FG.fc_begin(), CE = FG.fc_end();
       CI != CE; ++CI) {
    Instruction *Call = CI->getCallSite().getInstruction();

    // Loop over all of the actually called functions...
    callee_iterator I = callee_begin(Call), E = callee_end(Call);
    for (; I != E && I->first == Call; ++I) {
      assert(I->first == Call && "Bad callee construction!");
      if (I->second->size()) {
        DSGraph &Callee = getOrCreateGraph(*I->second);
        unsigned M;

        // Have we visited the destination function yet?
        std::unordered_map<DSGraph*, unsigned>::iterator It = ValMap.find(&Callee);
        if (It == ValMap.end())
          M = calculateSCCGraphs(Callee, Stack, NextID, ValMap);
        else
          M = It->second;
        if (M < Min) Min = M;
      }
    }
  }
  assert(ValMap[&FG] == MyID && "SCC construction assumption wrong!");
  if (Min != MyID)
    return Min;         // This is part of a larger SCC.

  // If this is a new SCC, process it now.
  bool IsMultiNodeSCC = false;
  while (Stack.back() != &FG) {
    DSGraph *NG = Stack.back();
    ValMap[NG] = ~0U;
    FG.cloneInto(*NG);

    // Update the DSInfo map and delete the old graph...
    for (DSGraph::retnodes_iterator I = NG->retnodes_begin();
         I != NG->retnodes_end(); ++I) {
      DSInfo[I->first] = &FG;
    }

    // Remove NG from the ValMap since the pointer may get recycled.
    ValMap.erase(NG);
    delete NG;
    Stack.pop_back();
    IsMultiNodeSCC = true;
  }

  // Clean up the graph before we start inlining a bunch again.
  if (IsMultiNodeSCC)
    FG.removeTriviallyDeadNodes();
  Stack.pop_back();
  processGraph(FG);
  ValMap[&FG] = ~0U;
  return MyID;
}

void CompleteBUDataStructures::processGraph(DSGraph &G) {
  std::unordered_set<Instruction*> calls;

  // The edges out of the current node are the call site targets...
  unsigned i = 0;
  for (DSGraph::fc_iterator CI = G.fc_begin(), CE = G.fc_end(); CI != CE;
       ++CI, ++i) {
    const DSCallSite &CS = *CI;
    Instruction *TheCall = CS.getCallSite().getInstruction();
    assert(calls.insert(TheCall).second &&
           "Call instruction occurs multiple times in graph??");

    // Fast path for noop calls.  Note that we don't care about merging globals
    // in the callee with nodes in the caller here.
    if (CS.getRetVal().isNull() && CS.getNumPtrArgs() == 0)
      continue;

    // Loop over all of the potentially called functions...
    // Inline direct calls as well as indirect calls because the direct
    // callee may have indirect callees and so may have changed.
    callee_iterator I = callee_begin(TheCall),E = callee_end(TheCall);
    unsigned TNum = 0, Num = 0;
    //DEBUG(Num = std::distance(I, E));
    for(; I != E; ++I, ++TNum) {
      assert(I->first == TheCall && "Bad callee construction!");
      Function *CalleeFunc = I->second;
      if(CalleeFunc->size()) {
        // Merge the callee's graph into this graph.  This works for normal
        // calls or for self recursion within an SCC.
        DSGraph &GI = getOrCreateGraph(*CalleeFunc);
        //++NumCBUInlines;
        G.mergeInGraph(CS, *CalleeFunc, GI,
                       DSGraph::StripAllocaBit | DSGraph::DontCloneCallNodes |
                       DSGraph::DontCloneAuxCallNodes);
        //DEBUG(std::cerr << "    Inlining graph [" << i << "/"
          //    << G.getFunctionCalls().size()-1
            //  << ":" << TNum << "/" << Num-1 << "] for "
            //  << CalleeFunc->getName() << "["
          //    << GI.getGraphSize() << "+" << GI.getAuxFunctionCalls().size()
            //  << "] into '" /*<< G.getFunctionNames()*/ << "' ["
            //  << G.getGraphSize() << "+" << G.getAuxFunctionCalls().size()
              //<< "]\n");
      }
    }
  }

  // Recompute the Incomplete markers
  G.maskIncompleteMarkers();
  G.markIncompleteNodes(DSGraph::MarkFormalArgs);

  // Delete dead nodes.  Treat globals that are unreachable but that can
  // reach live nodes as live.
  G.removeDeadNodes(DSGraph::KeepUnreachableGlobals);
}
