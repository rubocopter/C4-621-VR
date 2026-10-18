cbuffer Params : register(b0) {
    uint sw, sh, depthOffset, depthPitch;
    uint colorOffset, colorPitch, colorFormat;
    float eyeX;
    uint tw, th;
    float fx, a, b, wsign;
};
ByteAddressBuffer source : register(t0);
RWByteAddressBuffer closest : register(u0);
RWByteAddressBuffer winner : register(u1);
RWByteAddressBuffer rgba : register(u2);
RWByteAddressBuffer bgra : register(u3);
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
    return ProjectParallel(index,sw,sh,tw,th,d,fx,a,b,wsign,eyeX,target,depthKey);
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
