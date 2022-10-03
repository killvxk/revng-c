//
// Copyright rev.ng Labs Srl. See LICENSE.md for details.
//

#include "llvm/Support/raw_ostream.h"

#include "revng-c/Backend/DecompiledYAMLToC.h"
#include "revng-c/Support/PTMLC.h"

using namespace revng::pipes;

void printSingleCFile(llvm::raw_ostream &Out,
                      const DecompiledCCodeInYAMLStringMap &Functions,
                      const std::set<MetaAddress> &Targets) {
  auto Scope = ptml::Tag(ptml::tags::Div).scope(Out);
  // Print headers
  Out << helpers::includeQuote("revng-model-declarations.h")
      << helpers::includeQuote("revng-qemu-helpers-declarations.h") << "\n";

  if (Targets.empty()) {
    // If Targets is empty print all the Functions' bodies
    for (const auto &[MetaAddress, CFunction] : Functions)
      Out << CFunction << '\n';
  } else {
    // Otherwise only print the bodies of the Targets
    auto End = Functions.end();
    for (const auto &MetaAddress : Targets)
      if (auto It = Functions.find(MetaAddress); It != End)
        Out << It->second << '\n';
  }
}
