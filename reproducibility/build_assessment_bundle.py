"""Export the research-readiness assessment and append existing short reports.

No simulations are run and no source reports are rewritten.
"""
from pathlib import Path
from io import BytesIO
import hashlib
import json
import re
from xml.sax.saxutils import escape

from pypdf import PdfReader, PdfWriter
from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas
from reportlab.platypus import (
    SimpleDocTemplate, Paragraph, Table, TableStyle, Spacer, PageBreak, Image,
)
from PIL import Image as PILImage

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
OUT = HERE / 'output/pdf'
TMP = ROOT / 'tmp/pdfs/assessment_bundle'
OUT.mkdir(parents=True, exist_ok=True)
TMP.mkdir(parents=True, exist_ok=True)
ASSESSMENT = OUT / 'RESEARCH_READINESS_ASSESSMENT_RU.pdf'
BUNDLE = OUT / 'RESEARCH_ASSESSMENT_WITH_REPORTS_RU.pdf'
FONT_DIR = Path('C:/Windows/Fonts')
pdfmetrics.registerFont(TTFont('AssessmentArial', str(FONT_DIR / 'arial.ttf')))
pdfmetrics.registerFont(TTFont('AssessmentArialBold', str(FONT_DIR / 'arialbd.ttf')))
pdfmetrics.registerFontFamily('AssessmentArial', normal='AssessmentArial', bold='AssessmentArialBold')

INK = colors.HexColor('#21333e')
TEAL = colors.HexColor('#126d79')
MUTED = colors.HexColor('#536671')
PALE = colors.HexColor('#eaf3f4')
LINE = colors.HexColor('#c6d7dc')
WIDTH = 178 * mm
S = {
    'title': ParagraphStyle('title', fontName='AssessmentArialBold', fontSize=21, leading=25,
                            textColor=INK, spaceAfter=9),
    'meta': ParagraphStyle('meta', fontName='AssessmentArial', fontSize=9, leading=12,
                           textColor=MUTED, spaceAfter=12),
    'body': ParagraphStyle('body', fontName='AssessmentArial', fontSize=10.5, leading=14.2,
                           textColor=INK, spaceAfter=8),
    'head': ParagraphStyle('head', fontName='AssessmentArialBold', fontSize=12, leading=16,
                           textColor=TEAL, spaceBefore=9, spaceAfter=6, keepWithNext=True),
    'small': ParagraphStyle('small', fontName='AssessmentArial', fontSize=8.7, leading=11.6,
                            textColor=MUTED, spaceAfter=7),
    'cell': ParagraphStyle('cell', fontName='AssessmentArial', fontSize=9.5, leading=12.4,
                           textColor=INK),
    'th': ParagraphStyle('th', fontName='AssessmentArialBold', fontSize=9.5, leading=12.4,
                         textColor=colors.white),
}


def clean(text):
    return text.replace('\u2011', '-').replace('\u2013', '-').replace('\u2014', '-').replace('\u202f', ' ')


def p(text, style='body'):
    return Paragraph(clean(text), S[style])


def table(rows, widths, header=True):
    data = [[p(item, 'th' if header and i == 0 else 'cell') for item in row]
            for i, row in enumerate(rows)]
    obj = Table(data, colWidths=widths, repeatRows=1 if header else 0, hAlign='LEFT')
    commands = [
        ('VALIGN', (0, 0), (-1, -1), 'TOP'),
        ('LEFTPADDING', (0, 0), (-1, -1), 8), ('RIGHTPADDING', (0, 0), (-1, -1), 8),
        ('TOPPADDING', (0, 0), (-1, -1), 7), ('BOTTOMPADDING', (0, 0), (-1, -1), 7),
        ('LINEBELOW', (0, -1), (-1, -1), .5, LINE),
    ]
    if header:
        commands += [('BACKGROUND', (0, 0), (-1, 0), TEAL),
                     ('ROWBACKGROUNDS', (0, 1), (-1, -1), [colors.white, colors.HexColor('#f2f6f7')])]
    obj.setStyle(TableStyle(commands))
    return obj


def callout(text):
    obj = Table([[p(text)]], colWidths=[WIDTH])
    obj.setStyle(TableStyle([
        ('BACKGROUND', (0, 0), (-1, -1), PALE), ('BOX', (0, 0), (-1, -1), .6, LINE),
        ('LEFTPADDING', (0, 0), (-1, -1), 10), ('RIGHTPADDING', (0, 0), (-1, -1), 10),
        ('TOPPADDING', (0, 0), (-1, -1), 9), ('BOTTOMPADDING', (0, 0), (-1, -1), 3),
    ]))
    return obj


def write_pdf(path, story, title, label):
    def footer(c, doc):
        c.saveState()
        c.setStrokeColor(LINE)
        c.line(16*mm, 14*mm, 194*mm, 14*mm)
        c.setFont('AssessmentArial', 7.7)
        c.setFillColor(MUTED)
        c.drawString(16*mm, 10*mm, clean(label))
        c.drawRightString(194*mm, 10*mm, str(doc.page))
        c.restoreState()
    doc = SimpleDocTemplate(str(path), pagesize=A4, leftMargin=16*mm, rightMargin=16*mm,
                            topMargin=15*mm, bottomMargin=21*mm, title=title,
                            author='Network Studio', subject='Воспроизводимость и границы научного использования')
    doc.build(story, onFirstPage=footer, onLaterPages=footer)


assessment_story = [
    p('Оценка воспроизводимости<br/>и научного применения', 'title'),
    p('Network Studio | Windows CPU | 12 октября 2026 года', 'meta'),
    callout('<b>Конфигуратор пригоден для предварительных модельных исследований в проверенных режимах.</b> '
            'Для окончательных количественных выводов, особенно о синхронизации и обучении сети, нужны '
            'дополнительные исправления и проверки. Обоснованный статус - исследовательский прототип с проверенными компонентами.'),
    p('Что подтверждено проверками', 'head'),
    table([
        ['Уровень проверки', 'Оценка и основание'],
        ['Повторение собственных расчётов', 'Подтверждено для проверенных сценариев. В последней сборке '
         '28 регрессионных запусков дали 84 побайтно совпадающих CSV.'],
        ['Соответствие реализованным уравнениям', 'Хорошо подтверждено компонентными проверками. Независимый '
         'пересчёт парной и триплетной STDP по зарегистрированным событиям: расхождение не более 8,95 × 10<super>-14</super>.'],
        ['Численная точность всей симуляции', 'Подтверждена только для отдельных режимов; обнаружены ограничения и дефекты.'],
        ['Воспроизведение NeuroNet', 'Частичное: есть количественные и отдельные качественные расхождения.'],
        ['Биологическая достоверность', 'Представленными проверками не установлена. Нужна проверка на экспериментальных данных.'],
    ], [51*mm, 127*mm]),
    Spacer(1, 7),
    p('Побайтовое совпадение подтверждает повторяемость результата. Малое расхождение при пересчёте STDP '
      'подтверждает формулу обновления веса, но само по себе не подтверждает точность времён спайков.', 'small'),
    p('Оценка лабораторных работ', 'head'),
    p('<b>ЛР1.</b> Наиболее убедительное соответствие на уровне одиночного нейрона. Основные режимы подтверждены '
      'независимым решателем; кратность ответа зависит от шага.'),
    p('<b>ЛР2-3.</b> Воспроизводятся многие явления: циркуляция активности, пачки, фасилитация и депрессия. '
      'Точные периоды, амплитуды и некоторые сетевые режимы отличаются.'),
    p('<b>ЛР5.</b> Направления изменения веса совпали во всех шести опытах задания 1, но конечные веса различаются. '
      'В задании 2 есть качественные расхождения; устойчивое подавление обходного пути в задании 3 не получено.'),
    PageBreak(),
    p('Ограничения и условия применения', 'title'),
    p('Продолжение оценки | Состояние проверок на 12 октября 2026 года', 'meta'),
    p('Известные численные проблемы', 'head'),
    p('<b>1. Разрешение входных импульсов.</b> Импульс короче шага может исчезнуть либо передать избыточное воздействие. '
      'В контрольном опыте с шириной 10 мкс и шагом 25 мкс получалось 0% или 250% заданной площади импульса.'),
    p('<b>2. Устойчивость быстрых кинетик.</b> Быстрая затухающая переменная при некоторых разрешённых настройках RK4 '
      'численно растёт. Контрпример: скорость распада 200 000 с<super>-1</super> при шаге 25 мкс; '
      'программа может завершить такой расчёт без сообщения об ошибке.'),
    p('<b>3. Точность событий.</b> Регистрация спайка в конце шага накапливает ошибку времени. '
      'В одном FS-контроле она достигала 0,86 мс за 200 мс. В одном опыте STDP уменьшение шага вдвое '
      'изменило малый конечный вес примерно на 13%.'),
    p('Эти контрпримеры не отменяют результаты проверенных лабораторных режимов, но исключают утверждение '
      'о надёжности произвольной конфигурации.'),
    p('Область применимости выполненного аудита', 'head'),
    p('Полный аудит моделей выполнен до добавления триплетного правила, на CLI <b>4965bb5c…</b>. '
      'Для новой сборки <b>38e1a6a1…</b> проведены отдельные проверки пластичности и регрессии. '
      'Полный аудит HH и астроцитов повторно на новом бинарном файле не выполнялся.'),
    p('Что необходимо для исследования с публикацией', 'head'),
    p('Сначала следует исправить обработку границ стимулов и быстрых кинетик. Для выбранного опыта необходимо '
      'проверить сходимость по шагу, сравнить ключевые результаты с независимым решателем и оценить разброс '
      'по случайным реализациям. Для повторения нужно сохранять версию программы, граф сети, начальные '
      'состояния и все параметры.'),
    callout('<b>Совпадение с NeuroNet не является обязательным условием научной корректности.</b> '
            'Обязательны ясно заданная модель, проверенная реализация и контролируемая погрешность. '
            'Имеющиеся доказательства существенны, но пока недостаточны для заявления о полной валидации конфигуратора.'),
    p('Основания и материалы', 'head'),
    p('Пути относительно standalone/network_studio:<br/>'
      'model_audit/README_RU.md - математический аудит и численные контрпримеры;<br/>'
      'reproducibility/REPORT_RU.md - сводные проверки ЛР1-3;<br/>'
      'reproducibility/lab5/REPORT_RU.md - парная и триплетная STDP, регрессии новой сборки.', 'small'),
]
write_pdf(ASSESSMENT, assessment_story, 'Оценка воспроизводимости и научного применения Network Studio',
          'Network Studio | Оценка научного применения | 12.10.2026')
assert len(PdfReader(ASSESSMENT).pages) == 2


def inline_markdown(text):
    text = escape(clean(text))
    text = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', lambda m: m.group(1), text)
    text = re.sub(r'\*\*(.+?)\*\*', r'<b>\1</b>', text)
    text = re.sub(r'`([^`]+)`', r'\1', text)
    return text


def archive_report(source, destination, note, label):
    """Typeset the complete Markdown report, retaining its text, tables and image."""
    original_styles = dict(S)
    S['body'] = ParagraphStyle('archive_body', parent=S['body'], fontSize=10, leading=13, spaceAfter=6)
    S['title'] = ParagraphStyle('archive_title', parent=S['title'], fontSize=18, leading=22)
    S['meta'] = ParagraphStyle('archive_meta', parent=S['meta'], spaceAfter=9)
    lines = source.read_text(encoding='utf-8').splitlines()
    story = [p(lines[0].lstrip('# ').strip(), 'title'),
             p('Архивный краткий отчёт | Текст исходного документа сохранён', 'meta'),
             callout(note), Spacer(1, 9)]
    i = 1
    while i < len(lines):
        line = lines[i].strip()
        if not line:
            i += 1
            continue
        if line.startswith('#'):
            story.append(p(inline_markdown(line.lstrip('# ').strip()), 'head'))
            i += 1
        elif line.startswith('|'):
            rows = []
            while i < len(lines) and lines[i].strip().startswith('|'):
                cells = [v.strip() for v in lines[i].strip().strip('|').split('|')]
                if not all(re.fullmatch(r':?-+:?', cell) for cell in cells):
                    rows.append([inline_markdown(cell) for cell in cells])
                i += 1
            if len(rows[0]) == 2:
                widths = [37*mm, 141*mm]
            elif 'Программа' in rows[0][0]:
                widths = [94*mm, 33*mm, 51*mm]
            else:
                widths = [42*mm, 68*mm, 68*mm]
            story.extend([table(rows, widths), Spacer(1, 8)])
        elif line.startswith('!['):
            match = re.fullmatch(r'!\[([^\]]*)\]\(([^)]+)\)', line)
            img_path = source.parent / match.group(2)
            with PILImage.open(img_path) as img:
                w, h = img.size
            scale = min(WIDTH/w, 90*mm/h)
            story.extend([Image(str(img_path), width=w*scale, height=h*scale),
                          p(inline_markdown(match.group(1)), 'small')])
            i += 1
        else:
            paragraphs = [line]
            i += 1
            while i < len(lines) and lines[i].strip() and not lines[i].startswith(('#', '|', '![')):
                paragraphs.append(lines[i].strip())
                i += 1
            story.append(p(inline_markdown(' '.join(paragraphs))))
    write_pdf(destination, story, lines[0].lstrip('# ').strip(), label)
    S.update(original_styles)


lab1_pdf = TMP / 'LAB1_SHORT_ARCHIVE_RU.pdf'
lab2_pdf = TMP / 'LAB2_SHORT_ARCHIVE_RU.pdf'
archive_report(ROOT/'lab1/SHORT_REPORT_RU.md', lab1_pdf,
               '<b>Примечание к архивной версии.</b> Выводы относятся к проверенным режимам ЛР1. '
               'Позднее обнаружены численные ограничения, изложенные в основной оценке сборника. '
               'Указанный ниже метод NeuroNet отражает ранний отчёт; полные настройки всех исходных сеансов '
               'не восстановлены. Уточнённый охват проверки приведён в сводном отчёте ЛР1-3.',
               'Архивный краткий отчёт | ЛР1')
archive_report(ROOT/'lab2/SHORT_REPORT_RU.md', lab2_pdf,
               '<b>Примечание к архивной версии.</b> Указанный ниже предел интерфейса в 16 нейронов устарел: '
               'текущий GUI поддерживает 1000 нейронов и 50 000 связей. Дополнительный контроль кольца N=11 '
               'с TM дал циркуляцию; он включён в более поздний сводный отчёт ЛР1-3. Результаты ранних опытов сохранены.',
               'Архивный краткий отчёт | ЛР2')

parts = [
    ('Оценка воспроизводимости и научного применения', ASSESSMENT),
    ('Состав сборника и примечания к приложениям', TMP/'CONTENTS_RU.pdf'),
    ('Приложение 1. Краткий отчёт ЛР1 (архив)', lab1_pdf),
    ('Приложение 2. Краткий отчёт ЛР2 (архив)', lab2_pdf),
    ('Приложение 3. Сводный отчёт ЛР1-3', OUT/'LABS_1_3_REPRODUCIBILITY_RU.pdf'),
    ('Приложение 4. Краткий отчёт ЛР5', HERE/'lab5/output/pdf/LAB5_SHORT_REPORT_RU.pdf'),
]
counts = [len(PdfReader(path).pages) if index != 1 else 1 for index, (_, path) in enumerate(parts)]
starts = []
cursor = 1
for count in counts:
    starts.append(cursor)
    cursor += count
toc_rows = [['Документ', 'Страницы сборника']]
for (title, _), start, count in zip(parts, starts, counts):
    toc_rows.append([title, str(start) if count == 1 else f'{start}-{start+count-1}'])
contents = [p('Состав сборника', 'title'),
            p('Основная оценка и ранее подготовленные отчёты | 12 октября 2026', 'meta'),
            table(toc_rows, [139*mm, 39*mm]),
            p('Как читать приложения', 'head'),
            p('Первой приведена актуальная оценка научного применения. Затем приложены отдельные краткие '
              'отчёты ЛР1 и ЛР2, сводный отчёт ЛР1-3 и краткий отчёт ЛР5. Отдельного краткого документа ЛР3 '
              'нет: её результаты представлены в сводном отчёте. ЛР5 - четвёртая работа выполненной серии.'),
            p('Ранние краткие отчёты ЛР1 и ЛР2 переведены из Markdown в PDF с сохранением текста, таблиц '
              'и рисунка. Готовые PDF ЛР1-3 и ЛР5 включены целиком. Частичное повторение материала сохраняет '
              'полноту ранее подготовленных отчётов.'),
            p('Уточнения к архивным выводам', 'head'),
            p('<b>Версии.</b> Сводный отчёт ЛР1-3 и полный математический аудит относятся к CLI 4965bb5c…; '
              'актуальная сборка с триплетным правилом - 38e1a6a1…. Формулировка «текущая сборка» в архивных '
              'документах относится к моменту их проверки. Регрессия новой версии не является полным повтором прежнего аудита.'),
            p('<b>Ограничения.</b> Вывод об отсутствии ошибок в проверенных лабораторных режимах необходимо '
              'читать вместе с обнаруженными позднее дефектами узких импульсов и быстрых кинетик. '
              'Старый предел GUI в 16 нейронов больше не действует; контроль N=11 дополняет ранние кольцевые опыты.'),
            p('<b>ЛР5.</b> Сохранены оговорки об условном отнесении внешнего веса 0,0887917 к триплетному '
              'режиму и о длительности 12,525 с как разности отметок снимков, без полного журнала обучения.'),
            p('В PDF добавлены закладки разделов. Нижняя строка «Сборник» показывает общую нумерацию; '
              'внутренняя нумерация исходных приложений сохранена.', 'small')]
write_pdf(parts[1][1], contents, 'Состав сборника и примечания', 'Network Studio | Состав сборника')
assert len(PdfReader(parts[1][1]).pages) == 1

writer = PdfWriter()
page_cursor = 0
for title, path in parts:
    reader = PdfReader(path)
    writer.append(reader, import_outline=False)
    writer.add_outline_item(title, page_cursor)
    page_cursor += len(reader.pages)
total = len(writer.pages)
for index, page in enumerate(writer.pages):
    buffer = BytesIO()
    c = canvas.Canvas(buffer, pagesize=(float(page.mediabox.width), float(page.mediabox.height)))
    c.setFont('AssessmentArial', 7)
    c.setFillColor(MUTED)
    c.drawRightString(float(page.mediabox.width)-16*mm, 5*mm, f'Сборник | {index+1} / {total}')
    c.save()
    buffer.seek(0)
    page.merge_page(PdfReader(buffer).pages[0])
writer.add_metadata({'/Title': 'Оценка научного применения Network Studio и отчёты о воспроизводимости',
                     '/Author': 'Network Studio', '/Subject': 'ЛР1-3, ЛР5 и оценка исследовательской готовности'})
writer.page_mode = '/UseOutlines'
with BUNDLE.open('wb') as f:
    writer.write(f)

reader = PdfReader(BUNDLE)
assert len(reader.pages) == sum(counts)
assert len(reader.outline) == len(parts)
all_text = '\n'.join(page.extract_text() for page in reader.pages)
normalized_text = re.sub(r'\s+', ' ', all_text)
for token in ['28 регрессионных', '84 побайтно', '250%', '13%', '4965bb5c', '38e1a6a1',
              '1000 нейронов', '50 000 связей', '0,0887917', '12,525']:
    assert token in normalized_text, token
assert '\ufffd' not in all_text
assert '\u25a0' not in all_text

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

manifest = {
    'created': '2026-10-12', 'new_simulations': False,
    'outputs': [{'path': str(path), 'pages': len(PdfReader(path).pages), 'sha256': sha(path)}
                for path in [ASSESSMENT, BUNDLE]],
    'sections': [{'title': title, 'start': start, 'pages': count}
                 for (title, _), start, count in zip(parts, starts, counts)],
    'sources': [{'path': str(path), 'sha256': sha(path)} for path in [
        ROOT/'lab1/SHORT_REPORT_RU.md', ROOT/'lab2/SHORT_REPORT_RU.md',
        OUT/'LABS_1_3_REPRODUCIBILITY_RU.pdf', HERE/'lab5/output/pdf/LAB5_SHORT_REPORT_RU.pdf',
        ROOT/'model_audit/README_RU.md', HERE/'REPORT_RU.md', HERE/'lab5/REPORT_RU.md']],
}
(HERE/'assessment_bundle_manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps(manifest['outputs'], ensure_ascii=False, indent=2))
print('PASS: section bookmarks, page counts, Cyrillic and key claims verified')
