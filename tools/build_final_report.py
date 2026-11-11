"""Render docs/final/final_report.md into a copy of the CG2271 final report template.

Usage (from the repository root):
    python -I tools/build_final_report.py [--template PATH] [--out PATH]

The Markdown file is the source of truth; edit it and re-run this script instead of editing the
Word file. The template's title block, heading styles, page setup and footer are kept; its body
is replaced. Body text is 12 pt, 1.15 line spacing, justified (assignment format). Table text uses
TABLE_FONT_PT so the long pin / task / packet tables fit the page limit.

python-docx may be installed in the per-user site directory, which `python -I` ignores, so the
import below falls back to that directory explicitly.
"""
import argparse
import copy
import re
import site
import sys
from pathlib import Path

try:
    import docx
except ImportError:
    sys.path.append(site.getusersitepackages())
    import docx

from docx.enum.table import WD_TABLE_ALIGNMENT
from docx.enum.text import WD_ALIGN_PARAGRAPH, WD_COLOR_INDEX
from docx.oxml import parse_xml
from docx.oxml.ns import nsdecls, qn
from docx.shared import Cm, Pt

REPO = Path(__file__).resolve().parent.parent
DEFAULT_TEMPLATE = Path(r"D:\NUS\Year 3\Y3S1\CG2271\Labs\CG2271_Assg4\CG2271_As4_Final_Report_Template.docx")
SOURCE = REPO / "docs" / "final" / "final_report.md"
DEFAULT_OUT = REPO / "docs" / "final" / "B02_10_Final.docx"

BODY_FONT_PT = 12
TABLE_FONT_PT = 9
TABLE_GAP_PT = 6
LINE_SPACING = 1.15
PARA_SPACE_AFTER_PT = 4
PAGE_MARGIN_CM = 2.0           # template has 2.54 cm; 2 cm (as in the interim report) keeps the report within 10 pages
TEXT_WIDTH_CM = 21.0 - 2 * PAGE_MARGIN_CM
MIN_COLUMN_CM = 1.2
CM_PER_CHAR = 0.17            # average glyph width at TABLE_FONT_PT
CELL_PAD_CM = 0.35
MAX_FLOOR_CM = 4.5
MAX_IMAGE_WIDTH_CM = 12.5
MONO_FONT = "Consolas"
BULLET_ID = "90"               # unused abstractNum / numId in the template's numbering part
HEADER_SHADE = "D9D9D9"

HEADING_STYLES = {2: "Heading 2", 3: "Heading 3", 4: "Heading 4"}
REQUIRED_HEADINGS = [
    "Problem statement", "Sensors and Actuators used", "Hardware Architecture", "Software Architecture",
    "State Management (if applicable)", "Task Structure", "Coordination & Synchronisation",
    "Resource Protection", "Communication Protocol", "UART Configuration", "Packet description",
    "Online Function Used", "Additional Features", "Challenges faced & Solutions", "Conclusion",
]
FORBIDDEN_TEXT = ["DHT11", "B0X_YY", "Student 1", "A0123456X", "2025/26", "[Insert Image here]",
                  "1-2 lines here", "Dhruba"]
INLINE_RE = re.compile(r"(\*\*.+?\*\*|`[^`]+`|\[TODO:[^\]]*\])")
TODO_RE = re.compile(r"\[TODO:[^\]]*\]")


# ---------------------------------------------------------------- Markdown parsing
def split_front_matter(text: str) -> tuple[dict, str]:
    """Return ({'group','semester','members'}, body_text)."""
    meta = {"group": "", "semester": "", "members": []}
    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        return meta, text
    end = next(i for i in range(1, len(lines)) if lines[i].strip() == "---")
    for line in lines[1:end]:
        key, _, value = line.partition(":")
        value = value.strip()
        if key == "member":
            name, _, matric = value.partition("|")
            meta["members"].append((name.strip(), matric.strip()))
        elif key in meta:
            meta[key] = value
    return meta, "\n".join(lines[end + 1:])


def parse_table(lines: list[str]) -> list[list[str]]:
    rows = []
    for line in lines:
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if all(re.fullmatch(r":?-{3,}:?", c) for c in cells):
            continue
        rows.append(cells)
    return rows


def parse_blocks(body: str) -> list[tuple[str, object]]:
    """Yield (kind, payload) blocks: heading, para, bullets, table, image."""
    lines = body.splitlines()
    i = 0
    blocks = []
    while i < len(lines):
        line = lines[i].rstrip()
        if not line.strip():
            i += 1
        elif line.startswith("#"):
            level = len(line) - len(line.lstrip("#"))
            blocks.append(("heading", (level, line[level:].strip())))
            i += 1
        elif line.startswith("|"):
            j = i
            while j < len(lines) and lines[j].startswith("|"):
                j += 1
            blocks.append(("table", parse_table(lines[i:j])))
            i = j
        elif line.startswith("- "):
            items = []
            while i < len(lines) and lines[i].startswith("- "):
                items.append(lines[i][2:].strip())
                i += 1
            blocks.append(("bullets", items))
        elif re.fullmatch(r"!\[[^\]]*\]\([^)]+\)", line.strip()):
            m = re.fullmatch(r"!\[([^\]]*)\]\(([^)]+)\)", line.strip())
            blocks.append(("image", (m.group(1), m.group(2))))
            i += 1
        else:
            para = []
            while i < len(lines) and lines[i].strip() and not lines[i].startswith(("#", "|", "- ", "![")):
                para.append(lines[i].strip())
                i += 1
            blocks.append(("para", " ".join(para)))
    return blocks


# ---------------------------------------------------------------- run / paragraph helpers
def add_inline(paragraph, text: str, size_pt: int, bold: bool = False) -> None:
    """Add text with **bold**, `code` and highlighted [TODO: ...] markers."""
    for token in INLINE_RE.split(text):
        if not token:
            continue
        run_bold = bold
        mono = False
        todo = False
        if token.startswith("**") and token.endswith("**"):
            token, run_bold = token[2:-2], True
        elif token.startswith("`") and token.endswith("`"):
            token, mono = token[1:-1], True
        elif TODO_RE.fullmatch(token):
            todo = True
        run = paragraph.add_run(token)
        run.font.size = Pt(size_pt)
        run.bold = True if run_bold else None
        if mono:
            run.font.name = MONO_FONT
            run._element.rPr.rFonts.set(qn("w:hAnsi"), MONO_FONT)
        if todo:
            run.font.highlight_color = WD_COLOR_INDEX.YELLOW


def format_body(paragraph) -> None:
    fmt = paragraph.paragraph_format
    fmt.alignment = WD_ALIGN_PARAGRAPH.JUSTIFY
    fmt.line_spacing = LINE_SPACING
    fmt.space_before = Pt(0)
    fmt.space_after = Pt(PARA_SPACE_AFTER_PT)


def add_bullets(document, items: list[str]) -> None:
    for item in items:
        par = document.add_paragraph(style="List Paragraph")
        num_pr = parse_xml('<w:numPr %s><w:ilvl w:val="0"/><w:numId w:val="%s"/></w:numPr>'
                           % (nsdecls("w"), BULLET_ID))
        par._p.get_or_add_pPr().append(num_pr)
        format_body(par)
        par.paragraph_format.space_after = Pt(3)
        add_inline(par, item, BODY_FONT_PT)


def install_bullet_numbering(document) -> None:
    numbering = document.part.numbering_part.element
    abstract = parse_xml(
        '<w:abstractNum %s w:abstractNumId="%s"><w:multiLevelType w:val="hybridMultilevel"/>'
        '<w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="bullet"/><w:lvlText w:val="&#8226;"/>'
        '<w:lvlJc w:val="left"/><w:pPr><w:ind w:left="567" w:hanging="283"/></w:pPr></w:lvl>'
        '</w:abstractNum>' % (nsdecls("w"), BULLET_ID))
    first_num = numbering.find(qn("w:num"))
    if first_num is not None:
        first_num.addprevious(abstract)
    else:
        numbering.append(abstract)
    numbering.append(parse_xml('<w:num %s w:numId="%s"><w:abstractNumId w:val="%s"/></w:num>'
                               % (nsdecls("w"), BULLET_ID, BULLET_ID)))


# ---------------------------------------------------------------- tables
def column_widths(rows: list[list[str]]) -> list[float]:
    """Widths in cm: proportional to the text length, but never narrower than the longest word."""
    cols = len(rows[0])
    weights = []
    floors = []
    for c in range(cols):
        texts = [r[c] for r in rows if c < len(r)]
        lengths = [len(t) for t in texts]
        avg = sum(lengths) / len(lengths)
        weights.append(min(max(0.6 * avg + 0.4 * max(lengths), 10.0), 60.0))
        longest_word = max(len(w) for t in texts for w in (t.split() or [""]))
        floors.append(min(max(longest_word * CM_PER_CHAR + CELL_PAD_CM, MIN_COLUMN_CM), MAX_FLOOR_CM))
    total = sum(weights)
    widths = [TEXT_WIDTH_CM * w / total for w in weights]
    for _ in range(cols):  # raise narrow columns to their floor, take the difference from the others
        low = [i for i in range(cols) if widths[i] < floors[i]]
        if not low:
            break
        spare = sum(floors[i] - widths[i] for i in low)
        rest = sum(widths[i] for i in range(cols) if i not in low)
        widths = [floors[i] if i in low else widths[i] * (1 - spare / rest) for i in range(cols)]
    return widths


def shade(cell, fill: str) -> None:
    tc_pr = cell._tc.get_or_add_tcPr()
    tc_pr.append(parse_xml('<w:shd %s w:val="clear" w:color="auto" w:fill="%s"/>' % (nsdecls("w"), fill)))


def row_flags(row, header: bool) -> None:
    tr_pr = row._tr.get_or_add_trPr()
    tr_pr.append(parse_xml('<w:cantSplit %s/>' % nsdecls("w")))
    if header:
        tr_pr.append(parse_xml('<w:tblHeader %s/>' % nsdecls("w")))


def add_table(document, rows: list[list[str]]) -> None:
    ncols = len(rows[0])
    table = document.add_table(rows=len(rows), cols=ncols)
    table.style = "Table Grid"
    table.alignment = WD_TABLE_ALIGNMENT.CENTER
    table.autofit = False
    widths = column_widths(rows)
    for r, cells in enumerate(rows):
        row = table.rows[r]
        row_flags(row, header=(r == 0))
        for c in range(ncols):
            cell = row.cells[c]
            cell.width = Cm(widths[c])
            par = cell.paragraphs[0]
            par.paragraph_format.alignment = WD_ALIGN_PARAGRAPH.LEFT
            par.paragraph_format.space_after = Pt(0)
            par.paragraph_format.line_spacing = 1.0
            add_inline(par, cells[c] if c < len(cells) else "", TABLE_FONT_PT, bold=(r == 0))
            if r == 0:
                shade(cell, HEADER_SHADE)
    spacer = document.add_paragraph()  # small gap after the table (exact line height, no 12 pt blank line)
    spacer.paragraph_format.space_after = Pt(0)
    spacer.paragraph_format.line_spacing = Pt(TABLE_GAP_PT)


def add_image(document, alt: str, rel_path: str) -> None:
    path = (SOURCE.parent / rel_path).resolve()
    par = document.add_paragraph()
    format_body(par)
    par.paragraph_format.alignment = WD_ALIGN_PARAGRAPH.CENTER
    if path.is_file():
        par.add_run().add_picture(str(path), width=Cm(MAX_IMAGE_WIDTH_CM))
        return
    add_inline(par, "[TODO: insert image %s - %s]" % (rel_path, alt), BODY_FONT_PT)


# ---------------------------------------------------------------- title block
def set_cell_text(cell, text: str) -> None:
    par = cell.paragraphs[0]
    runs = par.runs
    if runs:
        runs[0].text = text
        for extra in runs[1:]:
            extra._element.getparent().remove(extra._element)
        run = runs[0]
    else:
        run = par.add_run(text)
    if TODO_RE.search(text):
        run.font.highlight_color = WD_COLOR_INDEX.YELLOW


def fill_title_block(document, meta: dict) -> None:
    paragraphs = document.paragraphs
    semester_runs = paragraphs[1].runs
    semester_runs[0].text = meta["semester"]
    for extra in semester_runs[1:]:
        extra._element.getparent().remove(extra._element)
    table = document.tables[0]
    set_cell_text(table.rows[0].cells[0], meta["group"])
    prototype = copy.deepcopy(table.rows[2]._tr)
    for row in list(table.rows)[2:]:
        table._tbl.remove(row._tr)
    for name, matric in meta["members"]:
        tr = copy.deepcopy(prototype)
        table._tbl.append(tr)
        row = table.rows[-1]
        set_cell_text(row.cells[0], name)
        set_cell_text(row.cells[1], matric)


def clear_body_after_title_table(document) -> None:
    body = document.element.body
    title_table = document.tables[0]._tbl
    node = title_table.getnext()
    while node is not None and node.tag != qn("w:sectPr"):
        nxt = node.getnext()
        body.remove(node)
        node = nxt


# ---------------------------------------------------------------- build
def render_blocks(document, blocks: list[tuple[str, object]]) -> None:
    for kind, payload in blocks:
        if kind == "heading":
            level, text = payload
            document.add_paragraph(text, style=HEADING_STYLES[level])
        elif kind == "para":
            par = document.add_paragraph(style="Normal")
            format_body(par)
            add_inline(par, payload, BODY_FONT_PT)
        elif kind == "bullets":
            add_bullets(document, payload)
        elif kind == "table":
            add_table(document, payload)
        elif kind == "image":
            add_image(document, *payload)


def build(template: Path, out: Path) -> None:
    meta, body = split_front_matter(SOURCE.read_text(encoding="utf-8"))
    document = docx.Document(str(template))
    for section in document.sections:
        section.left_margin = section.right_margin = Cm(PAGE_MARGIN_CM)
        section.top_margin = section.bottom_margin = Cm(PAGE_MARGIN_CM)
    fill_title_block(document, meta)
    clear_body_after_title_table(document)
    install_bullet_numbering(document)
    render_blocks(document, parse_blocks(body))
    out.parent.mkdir(parents=True, exist_ok=True)
    document.save(str(out))


# ---------------------------------------------------------------- self-check
def all_text(document) -> str:
    parts = [p.text for p in document.paragraphs]
    for table in document.tables:
        for row in table.rows:
            parts.extend(cell.text for cell in row.cells)
    return "\n".join(parts)


def check_body_format(document) -> list[str]:
    """Body paragraphs (Normal and bullets, after the title block) must be 12 pt, justified, 1.15."""
    bad = []
    for par in document.paragraphs[3:]:
        if par.style.name not in ("Normal", "List Paragraph") or not par.text.strip():
            continue
        sizes = {run.font.size.pt for run in par.runs if run.font.size is not None}
        fmt = par.paragraph_format
        ok = (sizes == {BODY_FONT_PT} or not par.runs) and fmt.alignment in (WD_ALIGN_PARAGRAPH.JUSTIFY, WD_ALIGN_PARAGRAPH.CENTER) \
            and abs((fmt.line_spacing or 0) - LINE_SPACING) < 0.001
        if not ok:
            bad.append(par.text[:50])
    return bad


def check(out: Path) -> bool:
    document = docx.Document(str(out))
    text = all_text(document)
    heads = {p.text for p in document.paragraphs if p.style.name.startswith("Heading")}
    problems = ["missing heading: " + h for h in REQUIRED_HEADINGS if h not in heads]
    problems += ["leftover text: " + f for f in FORBIDDEN_TEXT if f in text]
    problems += ["body format: " + b for b in check_body_format(document)]
    todos = TODO_RE.findall(text)
    words = len(re.findall(r"\S+", text))
    print("words (body + tables): %d" % words)
    print("tables: %d, paragraphs: %d" % (len(document.tables), len(document.paragraphs)))
    print("TODO markers: %d" % len(todos))
    for item in todos:
        print("  " + item)
    for problem in problems:
        print("PROBLEM: " + problem)
    return not problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--template", type=Path, default=DEFAULT_TEMPLATE)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    if not args.template.is_file():
        print("template not found: %s" % args.template, file=sys.stderr)
        return 1
    if not SOURCE.is_file():
        print("source not found: %s" % SOURCE, file=sys.stderr)
        return 1
    build(args.template, args.out)
    print("wrote %s" % args.out)
    return 0 if check(args.out) else 2


if __name__ == "__main__":
    sys.exit(main())
