int RunRenderProbe(bool self_test);
void SetRenderProbeModule();
#ifdef AC6_DEPTH_CAPTURE
bool DepthCaptureSucceeded();
#endif
int main() {
    SetRenderProbeModule();
    const int result=RunRenderProbe(true);
#ifdef AC6_DEPTH_CAPTURE
    if(!result && !DepthCaptureSucceeded()) return 2;
#endif
    return result;
}
