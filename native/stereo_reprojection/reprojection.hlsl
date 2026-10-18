cbuffer Params : register(b0) {
    uint sw, sh, tw, th;
    float fx, fy, a, b;
    float wsign, eyeX;
    uint padding0, padding1;
};
ByteAddressBuffer depth : register(t0);
ByteAddressBuffer color : register(t1);
RWByteAddressBuffer closest : register(u0);
RWByteAddressBuffer winner : register(u1);
RWByteAddressBuffer outputColor : register(u2);

[numthreads(256,1,1)]
void Clear(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= tw*th) return;
    closest.Store(4*tid.x, 0xffffffff);
    winner.Store(4*tid.x, 0xffffffff);
    outputColor.Store(4*tid.x, 0);
}

bool Project(uint index, out uint target, out uint depthKey) {
    target=0; depthKey=0;
    if (index >= sw*sh) return false;
    float d=asfloat(depth.Load(4*index));
    return ProjectParallel(index,sw,sh,tw,th,d,fx,a,b,wsign,eyeX,target,depthKey);
}

[numthreads(256,1,1)]
void DepthPass(uint3 tid : SV_DispatchThreadID) {
    uint target, bits;
    if (Project(tid.x,target,bits)) closest.InterlockedMin(4*target,bits);
}

[numthreads(256,1,1)]
void IndexPass(uint3 tid : SV_DispatchThreadID) {
    uint target, bits;
    if (Project(tid.x,target,bits) && bits==closest.Load(4*target))
        winner.InterlockedMin(4*target,tid.x);
}

[numthreads(256,1,1)]
void Resolve(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= tw*th) return;
    uint index=winner.Load(4*tid.x);
    // FXC may evaluate both arms of a ternary. Bound the LOAD address itself,
    // because root SRVs do not provide descriptor-based out-of-bounds checks.
    uint safeIndex=min(index,sw*sh-1u);
    uint selectedColor=color.Load(4*safeIndex);
    outputColor.Store(4*tid.x,index==0xffffffff ? 0 : selectedColor);
}
