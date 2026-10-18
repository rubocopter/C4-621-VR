// Shared by offline and live consumers; resource layout/loads stay in callers.
// Same source/target projection, parallel X translation, pixel centers.
bool ProjectParallel(uint index, uint sw, uint sh, uint tw, uint th,
                     float d, float fx, float a, float b, float wsign, float eyeX,
                     out uint target, out uint depthKey) {
    target=0;depthKey=0;
    if(index>=sw*sh || !isfinite(d) || d<0 || d>1) return false;
    float distance=wsign*b/(wsign*d-a);
    if(!isfinite(distance) || distance<=0) return false;
    float px=((index%sw)+0.5)*tw/sw-fx*eyeX*tw/(2*distance);
    float py=((index/sw)+0.5)*th/sh;
    if(!isfinite(px) || px<0 || px>=(float)tw || py<0 || py>=(float)th) return false;
    target=(uint)floor(py)*tw+(uint)floor(px);
    // Order by original depth, NOT reconstructed float32 distance (which can
    // collapse adjacent depth samples). d(distance)/d(depth) has sign -b.
    uint rawDepth=asuint(d)&0x7fffffffu; // Canonicalize negative zero.
    depthKey=b>0 ? 0x3f800000u-rawDepth : rawDepth;
    return true;
}
