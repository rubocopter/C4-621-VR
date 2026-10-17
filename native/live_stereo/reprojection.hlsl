cbuffer Params : register(b0) {
    uint sw, sh, depthOffset, depthPitch;
    uint colorOffset, colorPitch, colorFormat;
    float eyeX;
};
ByteAddressBuffer source : register(t0);
RWByteAddressBuffer closest : register(u0);
RWByteAddressBuffer winner : register(u1);
RWByteAddressBuffer rgba : register(u2);
RWByteAddressBuffer bgra : register(u3);
static const uint tw=1280, th=720;
[numthreads(256,1,1)]
void Clear(uint3 tid : SV_DispatchThreadID) {
    if(tid.x>=tw*th) return;
    closest.Store(4*tid.x,0xffffffff);winner.Store(4*tid.x,0xffffffff);
}
bool Project(uint index,out uint target,out uint depthKey) {
    target=0;depthKey=0;
    if(index>=sw*sh) return false;
    float d=asfloat(source.Load(depthOffset+(index/sw)*depthPitch+(index%sw)*4));
    if(!isfinite(d)||d<0||d>1) return false;
    // Fixed observed projection: fx=.572404087, A=-2.02655792e-6,
    // B=1.00000203, W=1. fy cancels for parallel horizontal eye shifts.
    float distance=1.00000203/(d+2.02655792e-6);
    if(!isfinite(distance)||distance<=0) return false;
    float px=((index%sw)+0.5)*tw/sw-0.572404087*eyeX*tw/(2*distance);
    float py=((index/sw)+0.5)*th/sh;
    if(!isfinite(px)||px<0||px>=(float)tw||py<0||py>=(float)th) return false;
    target=(uint)floor(py)*tw+(uint)floor(px);
    // Minimum ORIGINAL device-depth key, preserving adjacent floats.
    depthKey=0x3f800000u-(asuint(d)&0x7fffffffu);return true;
}
[numthreads(256,1,1)]
void DepthPass(uint3 tid : SV_DispatchThreadID) {
    uint target,key;if(Project(tid.x,target,key)) closest.InterlockedMin(4*target,key);
}
[numthreads(256,1,1)]
void IndexPass(uint3 tid : SV_DispatchThreadID) {
    uint target,key;
    if(Project(tid.x,target,key)&&key==closest.Load(4*target)) winner.InterlockedMin(4*target,tid.x);
}
[numthreads(256,1,1)]
void Resolve(uint3 tid : SV_DispatchThreadID) {
    if(tid.x>=tw*th) return;
    uint index=winner.Load(4*tid.x);
    // FXC can eagerly evaluate ternary arms: bound the Load address itself.
    uint safeIndex=min(index,sw*sh-1u);
    uint c=source.Load(colorOffset+(safeIndex/sw)*colorPitch+(safeIndex%sw)*4);
    uint r,g,b;
    if(colorFormat==24) {
        r=((c&1023)*255+511)/1023;g=(((c>>10)&1023)*255+511)/1023;b=(((c>>20)&1023)*255+511)/1023;
    } else if(colorFormat==87) { r=(c>>16)&255;g=(c>>8)&255;b=c&255; }
    else { r=c&255;g=(c>>8)&255;b=(c>>16)&255; }
    if(index==0xffffffff) r=g=b=0;
    rgba.Store(4*tid.x,r|(g<<8)|(b<<16)|0xff000000);
    bgra.Store(4*tid.x,b|(g<<8)|(r<<16)|0xff000000);
}
