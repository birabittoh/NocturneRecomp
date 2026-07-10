// Example single-pass RetroArch-style `.slang` custom post-process shader
// for shader_glsl_path, exercising the compatibility subset supported by
// rex::ui::CustomShader: `#pragma stage`, and the Source/vTexCoord/
// SourceSize/OutputSize/FragColor aliases.
//
// Note: only the fragment stage is compiled and run (the vertex stage below
// is provided for compatibility with real RetroArch shaders/tooling that
// expect one, but is ignored - see CustomShader's class comment); the
// built-in full-screen triangle strip vertex shader is used instead. The
// fixed preamble already declares Source (the input texture), vTexCoord
// (the UV for the current fragment) and FragColor (the output) - do not
// redeclare them here.
//
// FrameCount is currently always 0 (no per-frame counter is threaded through
// the paint flow), so effects that depend purely on it will not animate.

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;
void main() {
  gl_Position = Position;
  vTexCoord = TexCoord;
}

#pragma stage fragment
void main() {
  vec4 color = texture(Source, vTexCoord);

  // Simple scanline darkening, one scanline per rendered source pixel row -
  // fixed to the guest's actual rendered resolution (SourceSize), not
  // OutputSize (which changes with window/display scaling).
  float scanline = sin(vTexCoord.y * SourceSize.y * 3.14159265) * 0.5 + 0.5;
  float intensity = mix(0.75, 1.0, scanline);

  // Mild vignette using the normalized source-space coordinate.
  vec2 centered = vTexCoord * 2.0 - 1.0;
  float vignette = 1.0 - dot(centered, centered) * 0.15;

  FragColor = vec4(color.rgb * intensity * vignette, color.a);
}
