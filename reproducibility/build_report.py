"""Render the verified audit narrative to Markdown and a shareable PDF."""
import json
from html import escape
from pathlib import Path
import re
import sys

from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (SimpleDocTemplate, Paragraph, Spacer, Table,
                               TableStyle, PageBreak, Image, KeepTogether)

HERE = Path(__file__).resolve().parent
FONT = Path('C:/Windows/Fonts')
pdfmetrics.registerFont(TTFont('ArialAudit', str(FONT / 'arial.ttf')))
pdfmetrics.registerFont(TTFont('ArialAuditBold', str(FONT / 'arialbd.ttf')))
pdfmetrics.registerFontFamily('ArialAudit', normal='ArialAudit', bold='ArialAuditBold',
                            italic='ArialAudit', boldItalic='ArialAuditBold')
COLORS = {'ink': colors.HexColor('#182630'), 'muted': colors.HexColor('#4D5D69'),
          'teal': colors.HexColor('#137C79'), 'pale': colors.HexColor('#EEF5F5'),
          'line': colors.HexColor('#CFDADB')}
styles = {
    'title': ParagraphStyle('title', fontName='ArialAuditBold', fontSize=21, leading=25,
                            textColor=colors.black, spaceAfter=10),
    'heading': ParagraphStyle('heading', fontName='ArialAuditBold', fontSize=16, leading=20,
                              textColor=COLORS['ink'], spaceAfter=11),
    'subheading': ParagraphStyle('subheading', fontName='ArialAuditBold', fontSize=11, leading=14,
                                 textColor=COLORS['ink'], spaceBefore=7, spaceAfter=6),
    'body': ParagraphStyle('body', fontName='ArialAudit', fontSize=10, leading=14,
                           textColor=COLORS['ink'], spaceAfter=8, alignment=TA_LEFT),
    'small': ParagraphStyle('small', fontName='ArialAudit', fontSize=8.5, leading=11.5,
                            textColor=COLORS['muted'], spaceAfter=7),
    'cell': ParagraphStyle('cell', fontName='ArialAudit', fontSize=9, leading=12,
                           textColor=COLORS['ink']),
    'cellhead': ParagraphStyle('cellhead', fontName='ArialAuditBold', fontSize=9, leading=12,
                               textColor=COLORS['ink'])
}


def text_html(text):
    value = escape(str(text)).replace('\n', '<br/>')
    value = re.sub(r'\*\*(.*?)\*\*', r'<b>\1</b>', value)
    # Arial lacks the Unicode superscript minus; use supported glyphs in a raised run.
    superscripts = str.maketrans('⁰¹²³⁴⁵⁶⁷⁸⁹⁺⁻', '0123456789+-')
    value = re.sub(r'[⁰¹²³⁴⁵⁶⁷⁸⁹⁺⁻]+',
                   lambda match: '<super>' + match.group().translate(superscripts) + '</super>', value)
    return value


def footer(canvas, doc):
    canvas.saveState()
    canvas.setFont('ArialAudit', 8)
    canvas.setFillColor(COLORS['muted'])
    canvas.drawString(18*mm, 12*mm, 'Воспроизводимость лабораторных работ 1–3  |  12 октября 2026')
    canvas.drawRightString(192*mm, 12*mm, str(doc.page))
    canvas.restoreState()


def main():
    content = json.loads((HERE / 'report_content.json').read_text(encoding='utf-8'))
    pdf_dir = HERE / 'output/pdf'
    pdf_dir.mkdir(parents=True, exist_ok=True)
    path = pdf_dir / 'LABS_1_3_REPRODUCIBILITY_RU.pdf'
    doc = SimpleDocTemplate(str(path), pagesize=(210*mm, 297*mm),
                            rightMargin=18*mm, leftMargin=18*mm,
                            topMargin=17*mm, bottomMargin=20*mm,
                            title=content['title'], author='Network Studio')
    story = []
    md = [f"# {content['title']}", content['date']]
    for index, page in enumerate(content['pages']):
        if index:
            story.append(PageBreak())
        if index == 0:
            story.extend([Paragraph(text_html(content['title']), styles['title']),
                          Paragraph(text_html(content['date']), styles['small'])])
        else:
            story.append(Paragraph(text_html(page['title']), styles['heading']))
            md.append('## ' + page['title'])
        for block in page['blocks']:
            kind = block['kind']
            if kind in ('paragraph', 'small', 'subheading'):
                style = 'body' if kind == 'paragraph' else kind
                story.append(Paragraph(text_html(block['text']), styles[style]))
                md.append(('### ' if kind == 'subheading' else '') + block['text'])
            elif kind == 'bullets':
                for text in block['items']:
                    story.append(Paragraph('• ' + text_html(text), styles['body']))
                md.append('\n'.join('- ' + text for text in block['items']))
            elif kind == 'table':
                headers, rows = block['headers'], block['rows']
                data = [[Paragraph(text_html(v), styles['cellhead']) for v in headers]]
                data += [[Paragraph(text_html(v), styles['cell']) for v in row] for row in rows]
                widths = block.get('widths', [1]*len(headers))
                widths = [doc.width*v/sum(widths) for v in widths]
                table = Table(data, colWidths=widths, repeatRows=1, hAlign='LEFT')
                table.setStyle(TableStyle([
                    ('BACKGROUND', (0,0), (-1,0), COLORS['pale']),
                    ('LINEBELOW', (0,0), (-1,0), .6, COLORS['teal']),
                    ('LINEBELOW', (0,1), (-1,-1), .25, COLORS['line']),
                    ('VALIGN', (0,0), (-1,-1), 'TOP'),
                    ('LEFTPADDING', (0,0), (-1,-1), 6), ('RIGHTPADDING', (0,0), (-1,-1), 6),
                    ('TOPPADDING', (0,0), (-1,-1), 5), ('BOTTOMPADDING', (0,0), (-1,-1), 5)
                ]))
                story += [table, Spacer(1, 9)]
                md.append('\n'.join(['| ' + ' | '.join(str(v).replace('\n', '<br>') for v in headers) + ' |',
                                     '| ' + ' | '.join('---' for _ in headers) + ' |'] +
                                    ['| ' + ' | '.join(str(v).replace('\n', '<br>') for v in row) + ' |' for row in rows]))
            elif kind == 'figure':
                image_path = HERE / block['path']
                picture = Image(str(image_path))
                picture.drawHeight = doc.width*picture.imageHeight/picture.imageWidth
                picture.drawWidth = doc.width
                story.append(KeepTogether([picture, Paragraph(text_html(block['caption']), styles['small'])]))
                md.append(f"![{block['caption']}]({block['path']})")
            else:
                raise ValueError(kind)
    doc.build(story, onFirstPage=footer, onLaterPages=footer)
    (HERE / 'REPORT_RU.md').write_text('\n\n'.join(md) + '\n', encoding='utf-8')
    print(path)
    sys.path.insert(0, str(HERE.parents[1] / 'tmp/python_deps'))
    import pymupdf
    render_dir = HERE / 'qa'
    render_dir.mkdir(exist_ok=True)
    with pymupdf.open(path) as pdf:
        for number, page in enumerate(pdf, 1):
            page.get_pixmap(matrix=pymupdf.Matrix(1.35, 1.35)).save(render_dir / f'page_{number:02d}.png')
        print(f'Rendered {len(pdf)} pages')


if __name__ == '__main__':
    main()
