[shader("vertex")]
float4 vertexMain(float3x2 transform) : SV_Position { return float4(transform[0], 0, 1); }
