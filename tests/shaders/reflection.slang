struct Nested { float2 uv; uint2 indices; };
struct Input { float3 position; Nested nested; float2 values[2]; };
struct PushData { float a; float3 b; float4 c; };
[[vk::push_constant]] ConstantBuffer<PushData> parameters;
[shader("vertex")]
float4 vertexMain(Input input, float4 color, uint id : SV_VertexID) : SV_Position {
 return float4(input.position + float3(input.nested.uv, 0) + float3(input.values[0], 0), 1) + color * parameters.a;
}
[shader("fragment")]
float4 fragmentMain() : SV_Target { return parameters.c; }
[shader("compute")]
[numthreads(8, 4, 2)]
void computeMain(uint3 id : SV_DispatchThreadID) { }
