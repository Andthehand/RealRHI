struct Parameters {
    float time;
    float worker;
};
[[vk::push_constant]] ConstantBuffer<Parameters> parameters;

struct VertexOutput {
    float4 position : SV_Position;
    float3 color;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    const float2 positions[3] = {float2(0, -0.8), float2(0.7, 0.7), float2(-0.7, 0.7)};
    const float3 colors[3] = {float3(1, 0.15, 0.1), float3(0.1, 1, 0.25), float3(0.15, 0.3, 1)};
    float angle = parameters.time + parameters.worker + float(instanceId) * 0.07;
    float2 p = positions[vertexId] * 0.05;
    p = float2(p.x * cos(angle) - p.y * sin(angle), p.x * sin(angle) + p.y * cos(angle));
    float2 cell = float2(instanceId % 16, instanceId / 16);
    VertexOutput output;
    output.position = float4(p + (cell + 0.5) / 8.0 - 1.0, 0, 1);
    output.color = colors[(vertexId + uint(parameters.worker)) % 3];
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    return float4(input.color, 1);
}
