/* Minimal markdown renderer for wisp's docs/*.md.
   Covers exactly what those files use: h1-h3, paragraphs, indented (4-space)
   and fenced code blocks, pipe tables, ul/ol lists, and inline
   code/bold/italic/links. Links to *.md are rewritten to in-page routes. */

function esc(s) {
	return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function text(s) {
	// backslash escapes only apply outside code spans (CommonMark 6.1); the
	// escaped char is parked as \0<code>\0 so emphasis/links can't see it
	s = esc(s.replace(/\\([!-\/:-@\[-`{-~])/g,   // any ASCII punctuation
		(_, c) => "\0" + c.charCodeAt(0) + "\0"));
	s = s.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
	s = s.replace(/\*([^*]+)\*/g, "<em>$1</em>");
	// wikilinks [[page]] / [[page#slug]] route in-site; names are docs basenames
	s = s.replace(/\[\[([\w.-]+)(?:#([\w-]+))?\]\]/g, (_, p, h) =>
		`<a href="#${p}${h ? "/" + h : ""}">${p}</a>`);
	s = s.replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_, t, url) => {
		const m = url.match(/^([a-z-]+)\.md(?:#([\w-]+))?$/);
		if (m) return `<a href="#${m[1]}${m[2] ? "/" + m[2] : ""}">${t}</a>`;
		return `<a href="${url}" target="_blank" rel="noopener">${t}</a>`;
	});
	return s.replace(/\0(\d+)\0/g, (_, n) => esc(String.fromCharCode(+n)));
}

function inline(s) {
	let out = "";
	// split on code spans first so emphasis/link syntax inside them stays literal
	const re = /(`+)([^]*?)\1/g;
	let last = 0, m;
	while ((m = re.exec(s))) {
		out += text(s.slice(last, m.index)) + "<code>" + esc(m[2].trim()) + "</code>";
		last = re.lastIndex;
	}
	return out + text(s.slice(last));
}

function slug(s) {
	return s.toLowerCase().replace(/[^\w\s-]/g, "").trim().replace(/\s+/g, "-");
}

export function render(md) {
	const lines = md.split("\n");
	const out = [];
	let i = 0;
	while (i < lines.length) {
		const line = lines[i];

		if (/^\s*$/.test(line)) { i++; continue; }

		const h = line.match(/^(#{1,3})\s+(.*)/);
		if (h) {
			const lvl = h[1].length;
			out.push(`<h${lvl} id="${slug(h[2])}">${inline(h[2])}</h${lvl}>`);
			i++; continue;
		}

		if (line.startsWith("```")) {
			const buf = [];
			i++;
			while (i < lines.length && !lines[i].startsWith("```")) buf.push(lines[i++]);
			i++;
			out.push("<pre><code>" + esc(buf.join("\n")) + "</code></pre>");
			continue;
		}

		if (/^(?: {4}|\t)/.test(line)) {
			const buf = [];
			// blank lines inside stay part of the block if code follows
			while (i < lines.length && (/^(?: {4}|\t)/.test(lines[i]) ||
			       (/^\s*$/.test(lines[i]) && /^(?: {4}|\t)/.test(lines[i + 1] || "")))) {
				buf.push(lines[i].replace(/^(?: {4}|\t)/, ""));
				i++;
			}
			out.push("<pre><code>" + esc(buf.join("\n")) + "</code></pre>");
			continue;
		}

		if (/^\|/.test(line)) {
			const rows = [];
			while (i < lines.length && /^\|/.test(lines[i])) rows.push(lines[i++]);
			// split on unescaped pipes only; \| survives into the cell as a literal
			const cells = r => r.replace(/^\|/, "").replace(/(?<!\\)\|\s*$/, "")
				// GFM unescapes \| in a table cell even inside a code span
				.split(/(?<!\\)\|/).map(c => inline(c.trim().replace(/\\\|/g, "|")));
			const head = cells(rows[0]);
			const align = cells(rows[1]).map(c =>  // rows[1] is the |---| separator
				/^:-+:$/.test(c) ? " style=\"text-align:center\"" :
				/-+:$/.test(c) ? " style=\"text-align:right\"" : "");
			let html = "<div class=\"tw\"><table><thead><tr>";
			html += head.map((c, n) => `<th${align[n] || ""}>${c}</th>`).join("");
			html += "</tr></thead><tbody>";
			for (const r of rows.slice(2)) {
				const cs = cells(r);
				while (cs.length < head.length) cs.push("");
				html += "<tr>" + cs.slice(0, head.length)
					.map((c, n) => `<td${align[n] || ""}>${c}</td>`).join("") + "</tr>";
			}
			out.push(html + "</tbody></table></div>");
			continue;
		}

		const li = line.match(/^(\s*)([-*]|\d+\.)\s+/);
		if (li) {
			const tag = /\d/.test(li[2]) ? "ol" : "ul";
			const items = [];
			while (i < lines.length) {
				const m = lines[i].match(/^\s*(?:[-*]|\d+\.)\s+(.*)/);
				if (m) { items.push(m[1]); i++; }
				else if (/^\s{2,}\S/.test(lines[i])) { items[items.length - 1] += " " + lines[i].trim(); i++; }
				else break;
			}
			out.push(`<${tag}>` + items.map(t => `<li>${inline(t)}</li>`).join("") + `</${tag}>`);
			continue;
		}

		// paragraph: gather until blank or a block start
		const buf = [];
		while (i < lines.length && !/^\s*$/.test(lines[i]) &&
		       !/^(#{1,3}\s|```|\||(?: {4}|\t)|\s*(?:[-*]|\d+\.)\s)/.test(lines[i]))
			buf.push(lines[i++]);
		out.push("<p>" + inline(buf.join(" ")) + "</p>");
	}
	return out.join("\n");
}
