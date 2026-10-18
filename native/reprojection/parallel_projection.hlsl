// Shared by offline and live consumers; resource layout/loads stay in callers.
// Parallel X translation, pixel centers. Optional destination image intrinsics.
bool ProjectParallel(uint index, uint sw, uint sh, uint tw, uint th,
                     float d, float fx, float a, float b, float wsign, float eyeX,
                     float4 imageTransform,
                     out uint target, out uint depthKey) {
    target=0;depthKey=0;
    if(index>=sw*sh || !isfinite(d) || d<0 || d>1) return false;
    float distance=wsign*b/(wsign*d-a);
    if(!isfinite(distance) || distance<=0) return false;
    float px=((index%sw)+0.5)*tw/sw-fx*eyeX*tw/(2*distance);
    float py=((index/sw)+0.5)*th/sh;
    // Change focal scales/principal point AFTER projecting the translated eye
    // through source intrinsics. X scale also scales disparity to target fx.
    // Identical rays retain the original arithmetic at pixel boundaries.
    if(any(imageTransform!=float4(1,1,0,0))) {
        px=(px-tw*0.5)*imageTransform.x+(1+imageTransform.z)*tw*0.5;
        py=(py-th*0.5)*imageTransform.y+(1-imageTransform.w)*th*0.5;
    }
    if(!isfinite(px) || !isfinite(py) || px<0 || px>=(float)tw || py<0 || py>=(float)th) return false;
    target=(uint)floor(py)*tw+(uint)floor(px);
    // Order by original depth, NOT reconstructed float32 distance (which can
    // collapse adjacent depth samples). d(distance)/d(depth) has sign -b.
    uint rawDepth=asuint(d)&0x7fffffffu; // Canonicalize negative zero.
    depthKey=b>0 ? 0x3f800000u-rawDepth : rawDepth;
    return true;
}
// Offline v1 and existing callers retain identical source/target intrinsics.
bool ProjectParallel(uint index, uint sw, uint sh, uint tw, uint th,
                     float d, float fx, float a, float b, float wsign, float eyeX,
                     out uint target, out uint depthKey) {
    return ProjectParallel(index,sw,sh,tw,th,d,fx,a,b,wsign,eyeX,float4(1,1,0,0),target,depthKey);
}
