struct Parameters {
 float4 color;
 float4 depthAndSlope;
};
[[vk::push_constant]] ConstantBuffer<Parameters> parameters;

struct VertexOutput {
 float4 position : SV_Position;
 float pointSize : SV_PointSize;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
 float2 positions[] = {float2(-0.8, -0.8), float2(0.8, -0.8), float2(-0.8, 0.8), float2(0.8, 0.8)};
 float2 position = positions[vertexId];
 VertexOutput result;
 result.position = float4(position, parameters.depthAndSlope.x + position.x * parameters.depthAndSlope.y, 1);
 result.pointSize = 1;
 return result;
}

struct FragmentOutput {
 float4 first : SV_Target0;
 float4 second : SV_Target1;
};

[shader("fragment")]
FragmentOutput fragmentMain() {
 FragmentOutput result;
 result.first = parameters.color;
 result.second = parameters.color;
 return result;
}
