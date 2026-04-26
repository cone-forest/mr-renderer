// Binding 0: host fills [width, height, 0, 0] (uint32); Kompute tensor order must match C++.
StructuredBuffer<uint> g_dims;
RWStructuredBuffer<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 dtid : SV_DispatchThreadID) {
    uint w = g_dims[0];
    uint h = g_dims[1];
    if (dtid.x >= w || dtid.y >= h) {
        return;
    }
    uint idx = dtid.y * w + dtid.x;
    float u = w <= 1 ? 0.f : float(dtid.x) / float(w - 1);
    float v = h <= 1 ? 0.f : float(dtid.y) / float(h - 1);
    output[idx] = float4(u, v, 0.f, 1.f);
}
