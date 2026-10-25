struct Output { float4 position : SV_Position; float4 color; };
[shader("vertex")]
Output vertexMain(float2 position, float4 color) {
 Output result;
 result.position = float4(position, 0, 1);
 result.color = color;
 return result;
}
[shader("fragment")]
float4 fragmentMain(Output input) : SV_Target { return input.color; }
