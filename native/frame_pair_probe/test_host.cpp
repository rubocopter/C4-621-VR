#include <iostream>
int RunPairProbe(bool self_test,bool resize_test=false);
int main(int argc,char**) {
    const int result=RunPairProbe(true,argc>1);
    if(result) std::cerr<<"The paired GPU fixture did not recover matching color and depth\n";
    return result;
}
