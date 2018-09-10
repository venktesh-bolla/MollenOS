/* MollenOS
 *
 * Copyright 2018, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * MollenOS Terminal Implementation (Alumnious)
 * - The terminal emulator implementation for Vali. Built on manual rendering and
 *   using freetype as the font renderer.
 */
#pragma once

#include <string>
#include "terminal_freetype.hpp"

class CTerminalRenderer;

typedef struct FontGlyph {
    int             Stored;
    FT_UInt         Index;
    FT_Bitmap       Bitmap;
    FT_Bitmap       Pixmap;
    int             MinX;
    int             MaxX;
    int             MinY;
    int             MaxY;
    int             yOffset;
    int             Advance;
    unsigned long   Cached;
} FontGlyph_t;

class CTerminalFont
{
public:
    CTerminalFont(CTerminalFreeType& FreeType, CTerminalRenderer& Renderer, const std::string& FontPath, std::size_t InitialPixelSize);
    ~CTerminalFont();

    bool    SetSize(std::size_t PixelSize);
    int     RenderCharacter(int X, int Y, unsigned long Character);
    void    SetColor(uint8_t R, uint8_t G, uint8_t B, uint8_t A);

private:
    FT_Error    LoadGlyph(unsigned long Character, FontGlyph_t* Cached, int Want);
    FT_Error    FindGlyph(unsigned long Character, int Want);
    void        FlushGlyph(FontGlyph_t* Glyph);
    void        FlushCache();

private:
    CTerminalFreeType&  m_FreeType;
    CTerminalRenderer&  m_Renderer;
    FT_Face             m_Face;
    FontGlyph_t*        m_Current;
    FontGlyph_t         m_Cache[257]; /* 257 is a prime */

    int         m_FontHeight;
    int         m_FontWidth;
    int         m_Height;
    int         m_Ascent;
    int         m_Descent;
    int         m_LineSkip;
    std::size_t m_FontSizeFamily;
    uint8_t     m_BgR, m_BgG, m_BgB, m_BgA;

    int     m_FaceStyle;
    int     m_Style;
    int     m_Outline;
    int     m_Kerning;
    int     m_Hinting;
    FT_UInt m_PreviousIndex;

    int         m_GlyphOverhang;
    float       m_GlyphItalics;
    int         m_UnderlineOffset;
    int         m_UnderlineHeight;
    void*       m_Source;
    std::size_t m_SourceLength;
};