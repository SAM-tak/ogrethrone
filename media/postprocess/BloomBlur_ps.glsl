#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D source;
vulkan( layout( ogre_s0 ) uniform sampler sourceSampler );
vulkan( layout( ogre_P0 ) uniform Params { )
    uniform vec4 invTex0Size;
    uniform vec2 direction;
vulkan( }; )

vulkan_layout( location = 0 )
in block { vec2 uv0; } inPs;
vulkan_layout( location = 0 )
out vec4 fragColour;

void main()
{
    // Normalized nine-tap Gaussian, paired into five bilinear samples.
    vec2 stepSize = invTex0Size.xy * direction;
    vec3 colour = texture( vkSampler2D( source, sourceSampler ), inPs.uv0 ).rgb * 0.2270270270;
    colour += texture( vkSampler2D( source, sourceSampler ), inPs.uv0 + stepSize * 1.3846153846 ).rgb * 0.3162162162;
    colour += texture( vkSampler2D( source, sourceSampler ), inPs.uv0 - stepSize * 1.3846153846 ).rgb * 0.3162162162;
    colour += texture( vkSampler2D( source, sourceSampler ), inPs.uv0 + stepSize * 3.2307692308 ).rgb * 0.0702702703;
    colour += texture( vkSampler2D( source, sourceSampler ), inPs.uv0 - stepSize * 3.2307692308 ).rgb * 0.0702702703;
    fragColour = vec4( colour, 1.0 );
}
