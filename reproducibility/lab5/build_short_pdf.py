"""One-page short report from the checked lab5 evidence; no new simulation."""
from pathlib import Path
import json
from reportlab.lib import colors
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import SimpleDocTemplate, Paragraph, Table, TableStyle, Spacer
from pypdf import PdfReader

HERE=Path(__file__).resolve().parent
OUT=HERE/'output/pdf/LAB5_SHORT_REPORT_RU.pdf'
OUT.parent.mkdir(parents=True,exist_ok=True)
rows=json.loads((HERE/'updated/findings_all.json').read_text(encoding='utf-8'))['rows']
cases={r['case']:r for r in rows if r['workload']=='official' and r['dt_us']==25}
assert len(cases)==14
font=Path('C:/Windows/Fonts')
pdfmetrics.registerFont(TTFont('ReportArial',str(font/'arial.ttf')))
pdfmetrics.registerFont(TTFont('ReportArialBold',str(font/'arialbd.ttf')))
pdfmetrics.registerFontFamily('ReportArial',normal='ReportArial',bold='ReportArialBold')
ink=colors.HexColor('#20323d');teal=colors.HexColor('#126d79');muted=colors.HexColor('#52636e')
styles={
    'title':ParagraphStyle('title',fontName='ReportArialBold',fontSize=20,leading=24,textColor=ink,spaceAfter=5),
    'meta':ParagraphStyle('meta',fontName='ReportArial',fontSize=9,leading=12,textColor=muted,spaceAfter=12),
    'body':ParagraphStyle('body',fontName='ReportArial',fontSize=10,leading=13,textColor=ink,spaceAfter=7),
    'head':ParagraphStyle('head',fontName='ReportArialBold',fontSize=11,leading=14,textColor=teal,spaceBefore=5,spaceAfter=5),
    'small':ParagraphStyle('small',fontName='ReportArial',fontSize=8.3,leading=10.8,textColor=muted,spaceAfter=6),
    'cell':ParagraphStyle('cell',fontName='ReportArial',fontSize=9,leading=11.5,textColor=ink),
    'th':ParagraphStyle('th',fontName='ReportArialBold',fontSize=9,leading=11.5,textColor=colors.white),
}
def p(s,kind='body'):return Paragraph(s,styles[kind])
story=[p('Лабораторная работа №5','title'),
    p('Краткий отчёт о воспроизводимости | 12 октября 2026<br/>Четвёртая работа выполненной серии. Network Studio и NeuroNet SM R05.','meta')]
callout=Table([[p('<b>Вывод: воспроизводимость частичная.</b> Направления изменений парной STDP совпадают, но численные веса отличаются. В триплетных опытах есть также качественные расхождения.')]],colWidths=[179*mm])
callout.setStyle(TableStyle([('BACKGROUND',(0,0),(-1,-1),colors.HexColor('#eaf3f4')),
    ('BOX',(0,0),(-1,-1),.6,colors.HexColor('#c4dcdf')),('LEFTPADDING',(0,0),(-1,-1),9),
    ('RIGHTPADDING',(0,0),(-1,-1),9),('TOPPADDING',(0,0),(-1,-1),8),('BOTTOMPADDING',(0,0),(-1,-1),3)]))
story += [callout,Spacer(1,7),p('Что сделано','head'),
    p('В конфигуратор добавлены минимальное триплетное правило STDP и таблица импульсов внутри периода. Проверены 14 протоколов при шагах 25 и 12,5 мкс; сетевой опыт повторён с 10 реализациями шума на каждую из трёх схем. Основные стимулы: 25 единиц модели × 3 мс, исходный вес 0,1, обучение 100 с. Выбор правила и стимуляция сохраняются в JSON.'),
    p('Сопоставление результатов','head'),
    p('Выбранные конечные веса; Studio: шаг 25 мкс, 100 с. Первые две строки относятся к заданию 1, остальные - к заданию 2.','small')]
selected=[
 ('10 Гц, пре → пост; парное','t1_pre_post_10Hz'),
 ('10 Гц, пост → пре; парное','t1_post_pre_10Hz'),
 ('Пары, 10 Гц; парное','t2a_pre_post_10Hz'),
 ('Пары, 10 Гц; триплетное','t2a_triplet_pre_post_10Hz'),
 ('Пре → пост → пре, 1 Гц; триплетное*','t2b_triplet_pre_post_pre_1Hz'),
 ('Пост → пре → пост, 1 Гц; триплетное','t2b_triplet_post_pre_post_1Hz')]
table=[[p('Протокол','th'),p('NeuroNet','th'),p('Studio','th')]]
def number(x):return f'{x:.8f}'.rstrip('0').rstrip('.').replace('.',',')
for label,key in selected:
    r=cases[key];table.append([p(label,'cell'),p(number(r['external_report_weight']),'cell'),p(number(r['final_weight']),'cell')])
t=Table(table,colWidths=[111*mm,34*mm,34*mm],hAlign='LEFT')
t.setStyle(TableStyle([('BACKGROUND',(0,0),(-1,0),teal),('VALIGN',(0,0),(-1,-1),'TOP'),
    ('ROWBACKGROUNDS',(0,1),(-1,-1),[colors.white,colors.HexColor('#f2f5f6')]),
    ('LINEBELOW',(0,0),(-1,0),.7,teal),('LINEBELOW',(0,-1),(-1,-1),.5,colors.HexColor('#cad5d9')),
    ('LEFTPADDING',(0,0),(-1,-1),7),('RIGHTPADDING',(0,0),(-1,-1),7),
    ('TOPPADDING',(0,0),(-1,-1),6),('BOTTOMPADDING',(0,0),(-1,-1),6)]))
story += [t,Spacer(1,5),p('*Результат NeuroNet 0,0887917 отнесён к триплетному режиму предварительно: неизменность флажка на протяжении всего опыта не подтверждена.','small'),
    p('<b>Задание 1:</b> знаки изменений совпали во всех шести опытах. <b>Задание 2:</b> при парной стимуляции 10 Гц Studio даёт больший вес для триплетного правила, а NeuroNet - меньший; внешнее ослабление для пре → пост → пре не воспроизведено.'),
    p('<b>Задание 3:</b> прямая связь усиливается, устойчивое подавление обходного пути не получено. За 5 с её вес в Studio: среднее 0,101444, диапазон 0,100323-0,103909. При 12,525 с: среднее 0,103344 против внешнего 0,106812. Длительность 12,525 с взята из разности отметок снимков и не восстанавливает полный журнал исходного обучения.'),
    p('Причины расхождений и проверка корректности','head'),
    p('При одинаковом импульсе 25 × 3 мс различаются фактические последовательности спайков: в Studio при 1 Гц возникают дублеты. Кроме того, различаются стартовые состояния, шаг и условия проведения сигналов; шум и внутренний порядок событий NeuroNet полностью не восстановлены. Уменьшение шага вдвое не устранило расхождения.'),
    p('Все <b>4/4 набора автотестов</b> пройдены. Независимый пересчёт весов по спайкам для 36 новых запусков дал расхождение не более <b>8,95 × 10<super>-14</super></b>. Это подтверждает реализацию выбранной формулы на зарегистрированной активности, но не тождество моделей NeuroNet и Studio.'),
    p('Источники: пособие С. А. Лобова и др., 2023, с. 8-9, 24-27; отчёт ЛР4 и сохранённые состояния NeuroNet. Полные таблицы, параметры, конфигурации и контрольные суммы: reproducibility/lab5/REPORT_RU.md и приложения к нему.','small')]

def footer(canvas,doc):
    canvas.saveState();canvas.setStrokeColor(colors.HexColor('#cad5d9'))
    canvas.line(15.5*mm,13.5*mm,194.5*mm,13.5*mm)
    canvas.setFont('ReportArial',7.6);canvas.setFillColor(muted)
    canvas.drawString(15.5*mm,9.5*mm,'Network Studio | Windows CPU | CLI SHA256: 38e1a6a15f35…')
    canvas.drawRightString(194.5*mm,9.5*mm,str(doc.page));canvas.restoreState()
doc=SimpleDocTemplate(str(OUT),pagesize=A4,leftMargin=15.5*mm,rightMargin=15.5*mm,
    topMargin=13*mm,bottomMargin=19*mm,title='ЛР №5 - краткий отчёт о воспроизводимости',
    author='Network Studio',subject='STDP, триплетное правило и сравнение результатов с NeuroNet')
doc.build(story,onFirstPage=footer,onLaterPages=footer)
reader=PdfReader(OUT)
assert len(reader.pages)==1,f'Expected one concise page, got {len(reader.pages)}'
text=reader.pages[0].extract_text()
for key in ['воспроизводимость частичная','0,0887917','12,525','4/4','36 новых']:
    assert key in text,key
assert '\ufffd' not in text
print(str(OUT));print('PASS: 1 page, key values and Cyrillic text extracted')
