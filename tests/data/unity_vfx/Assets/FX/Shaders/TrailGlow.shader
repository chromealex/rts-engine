// Synthetic custom shader for the importer tests: additive glow with a boost factor.
Shader "Custom/TrailGlow" {
    Properties { _MainTex ("Texture", 2D) = "white" {} _Color ("Color", Color) = (1,1,1,1) _Boost ("Boost", Float) = 1 }
    SubShader {
        Tags { "Queue"="Transparent" "RenderType"="Transparent" }
        Blend SrcAlpha One
        ZWrite Off Cull Off
        Pass {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"
            sampler2D _MainTex; fixed4 _Color; float _Boost;
            struct v2f { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; fixed4 col : COLOR; };
            v2f vert (appdata_full v) { v2f o; o.pos = UnityObjectToClipPos(v.vertex); o.uv = v.texcoord.xy; o.col = v.color; return o; }
            fixed4 frag (v2f i) : SV_Target { return tex2D(_MainTex, i.uv) * i.col * _Color * _Boost; }
            ENDCG
        }
    }
}
