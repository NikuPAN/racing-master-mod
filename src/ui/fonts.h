#pragma once

// The panel's fonts: Windows' own font files, memory-mapped (never copied to the heap), merged
// into one font so that Traditional Chinese, Simplified Chinese, Japanese, Korean, Hong Kong
// characters, symbols and Latin all render whichever panel language is chosen. With Dear ImGui
// 1.92 and later, glyphs are rasterised the first time they are drawn, so no glyph ranges are
// listed and changing the text size reloads nothing.

// Adds the font chain to the current ImGui context's atlas. Returns how many of the seven system
// fonts were found; 0 means the embedded Latin-only font is in use. Logs each missing file once.
int LoadFontChain();

// Called between frames. If the glyph atlas has grown past 4096 pixels in either direction (a very
// long list of never-seen characters scrolled end to end), the fonts are cleared, reloaded and
// the cache compacted, which replaces the texture with a fresh small one. Without the compact
// call nothing shrinks.
void FontAtlasValve();
