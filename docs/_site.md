# Site format

Rules every page in docs/ must follow. The site is a generic markdown renderer:
left sidebar lists pages grouped in sections (template library pinned at the
bottom), center column renders one page, right column is a jump-map of that
page's headings. Every sidebar section is collapsible.

## Page rules

- Exactly one H1, on line 1 of the file. It is the sidebar title.
- Only H2 and H3 below that. H2s appear in the right-column page map.
- Anchors are GitHub slugs: lowercase, spaces to `-`, punctuation dropped.
- Cross-page links are wikilinks: `[[file]]` or `[[file#heading]]`, where
  `file` is the basename without `.md` and `heading` is the GitHub slug.
- Plain markdown only: paragraphs, tables, lists, code fences. No HTML,
  no footnotes, no images.
- Code fences for wisp config are tagged `wisp`; shell is `sh`.
- Every page ends with a `## Gotchas` section, one line per item.
- Most important information first, on the page and in each section.

## Nav

`_nav.json` is the sidebar: an array of sections, each
`{"section": "Name", "pages": ["file", ...]}`. Filenames without `.md`,
in display order. The last section is the template library.

## Readability

Evidence-backed rules the page CSS must follow (sources: Dyson & Haselgrove
2001 on line length; Rello & Baeza-Yates 2016/2017 on fonts and backgrounds;
WCAG 2.2 1.4.8; A List Apart zebra-striping studies; NN/g link guidelines):

- Prose column: `max-width: 68ch` (55–75 cpl reads fastest, ≤80 hard cap).
  Code blocks and tables may exceed it with their own `overflow-x`.
- Body: 17px sans-serif, line-height 1.6–1.65, left-aligned, never justified,
  no letter-spacing tweaks on body text, no long italic passages.
- Not pure #fff on #000 — that maximises halation for astigmatic readers.
  Off-white (~#e4e6e9) on near-black (~#121417), contrast 12:1–16:1; dim
  text stays ≥7:1.
- Paragraph separation by space (~1.25em), never indent. Headings: sizes
  ~1.2–1.25× per level, h1 ≤ 2× body, and visibly more space above
  (~2.2em) than below (~0.6em) so they bind to their section.
- Code at 0.9em of body (mono optically outsizes sans), pre line-height ~1.45.
- Tables: horizontal rules only (2px under the header), no vertical rules,
  ~.6em/.9em cell padding, zebra at a just-perceptible ~3% lightness lift.
- Links keep their underline (offset ~.15em, 1px, thicker on hover) and get a
  light desaturated hue that clears 4.5:1 on the background.
