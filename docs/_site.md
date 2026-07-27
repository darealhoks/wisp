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
