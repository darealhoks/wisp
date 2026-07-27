// ponytail: smoke test — run `node test-md.mjs` from web/
import { render } from "./md.js";
import { readFileSync } from "fs";

for (const f of ["index", "install", "first-config", "syntax", "types", "builtins", "state", "modules", "wispctl", "gotchas", "templates"]) {
	const html = render(readFileSync("../docs/" + f + ".md", "utf8"));
	const counts = {};
	for (const m of html.matchAll(/<(h[123]|pre|table|ul|ol|p)[ >]/g))
		counts[m[1]] = (counts[m[1]] || 0) + 1;
	console.log(f, JSON.stringify(counts));
	if (/<script/.test(html)) throw new Error("unescaped html leaked");
}
const h = render(readFileSync("../docs/syntax.md", "utf8"));
console.assert(h.includes("<table>"), "table");
console.assert(render("see [[install#build-knobs]]").includes('href="#install/build-knobs"'), "wikilink rewrite");
console.assert(render("**b** and `c`").includes("<strong>b</strong>"), "bold");
console.assert(render("    a\n\n    b").split("<pre>").length === 2, "blank line inside code block");
console.log("ok");
