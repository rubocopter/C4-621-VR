#include <iostream>
int RunLivePairProbe(bool,bool);
int main(int argc,char**) { const auto result=RunLivePairProbe(true,argc>1); if(result) std::cerr<<"Continuous paired GPU fixture failed\n"; return result; }
