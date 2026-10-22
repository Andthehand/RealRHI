[require(spvDescriptorHeapEXT)]
[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint> output = RWStructuredBuffer<uint>.Handle(uint2(0, 0));
    output[threadId.x] = 2 * threadId.x + 1;
}
