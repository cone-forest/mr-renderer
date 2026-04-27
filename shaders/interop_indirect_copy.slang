struct DrawIndexedCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

StructuredBuffer<DrawIndexedCommand> g_source;
RWStructuredBuffer<DrawIndexedCommand> g_destination;

[shader("compute")]
[numthreads(1, 1, 1)]
void compute_main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x > 0) {
        return;
    }

    g_destination[0] = g_source[0];
}
