/* Minimal markdown renderer for wisp's docs/*.md.
   Covers exactly what those files use: h1-h3, paragraphs, indented (4-space)
   and fenced code blocks, pipe tables, ul/ol lists, and inline
   code/bold/italic/links. Links to *.md are rewritten to in-page routes. */

function esc(s) {
	return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function inline(s) {
	s = esc(s);
	s = s.replace(/`([^`]+)`/g, (_, c) => "<code>" + c + "</code>");
	s = s.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
	s = s.replace(/\*([^*]+)\*/g, "<em>$1</em>");
	// wikilinks [[page]] / [[page#slug]] route in-site; names are docs basenames
	s = s.replace(/\[\[([\w.-]+)(?:#([\w-]+))?\]\]/g, (_, p, h) =>
		`<a href="#${p}${h ? "/" + h : ""}">${p}</a>`);
	s = s.replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_, t, url) => {
		const m = url.match(/^([a-z]+)\.md$/);
		if (m) return `<a href="#${m[1]}">${t}</a>`;
		return `<a href="${url}" target="_blank" rel="noopener">${t}</a>`;
	});
	return s;
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
			const cells = r => r.replace(/^\||\|$/g, "").split("|").map(c => inline(c.trim()));
			let html = "<table><thead><tr>";
			html += cells(rows[0]).map(c => `<th>${c}</th>`).join("");
			html += "</tr></thead><tbody>";
			for (const r of rows.slice(2))  // rows[1] is the |---| separator
				html += "<tr>" + cells(r).map(c => `<td>${c}</td>`).join("") + "</tr>";
			out.push(html + "</tbody></table>");
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
