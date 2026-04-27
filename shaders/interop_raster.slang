struct VSInput {
    float2 position : POSITION;
    float3 color : COLOR0;
};

struct VSOutput {
    float4 position : SV_Position;
    float3 color : COLOR0;
};

[shader("vertex")]
VSOutput vertex_main(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragment_main(VSOutput input) : SV_Target0
{
    return float4(input.color, 1.0);
}
