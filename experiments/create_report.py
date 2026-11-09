from docx import Document
from docx.shared import Cm, Pt
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path
import csv, statistics, copy
p=Path(__file__).resolve().parent; root=p.parent
sample=Path('/Users/german/453504/OSISP/LAB4NEW/Отчёт_ЛР4_вариант4.docx')
d=Document(sample)
body=d.element.body
for e in list(body)[32:-1]: body.remove(e)
for para in d.paragraphs:
 if 'к лабораторной работе №4' in para.text:
  for run in para.runs: run.text=run.text.replace('№4','№5')
 if 'ВЗАИМОДЕЙСТВИЕ ПРОЦЕССОВ' in para.text:
  for run in para.runs: run.text=''
  r=para.runs[0];r.text='«ЭЛЕМЕНТЫ СЕТЕВОГО ПРОГРАММИРОВАНИЯ»';r.bold=True
normal=d.styles['Normal'];normal.font.name='Times New Roman';normal.font.size=Pt(14)
normal.paragraph_format.line_spacing=1.0

def para(text='',bold=False,center=False,indent=True):
 q=d.add_paragraph();q.alignment=WD_ALIGN_PARAGRAPH.CENTER if center else WD_ALIGN_PARAGRAPH.JUSTIFY
 q.paragraph_format.first_line_indent=Cm(1.25 if indent and not center else 0);q.paragraph_format.space_before=Pt(0);q.paragraph_format.space_after=Pt(0);q.paragraph_format.line_spacing=1
 r=q.add_run(text);r.bold=bold;return q

def heading(t):
 q=para(t,True);q.paragraph_format.keep_with_next=True
 q.paragraph_format.space_before=Pt(12);q.paragraph_format.space_after=Pt(12)

def table(headers, rows):
 t=d.add_table(rows=1,cols=len(headers));t.style='Table Grid'
 for c,v in zip(t.rows[0].cells,headers):c.text=str(v)
 for row in rows:
  for c,v in zip(t.add_row().cells,row):c.text=str(v)
 for row in t.rows:
  for c in row.cells:
   for q in c.paragraphs:
    q.paragraph_format.first_line_indent=Cm(0);q.paragraph_format.space_after=Pt(0)
    for r in q.runs:r.font.size=Pt(11)
 return t

def page():d.add_page_break()
def image(path,width=15):
 q=para('',center=True);q.add_run().add_picture(str(path),width=Cm(width))
# Diagram rendered from the separately supplied Mermaid description.
im=Image.new('RGB',(1300,900),'white');dr=ImageDraw.Draw(im)
try:f=ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf',25); small=ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf',21)
except OSError:f=small=ImageFont.load_default()
xs=[150,650,1150]
for x,label in zip(xs,['Alice','TCP-сервер','Bob']):dr.rectangle((x-110,30,x+110,80),outline='black',width=2);dr.text((x-65,40),label,font=f,fill='black');dr.line((x,80,x,865),fill='#888888',width=2)
def arrow(a,b,y,s):
 x1,x2=xs[a],xs[b];dr.line((x1,y,x2,y),fill='black',width=3);dr.polygon([(x2,y),(x2-(12 if x2>x1 else -12),y-7),(x2-(12 if x2>x1 else -12),y+7)],fill='black');dr.text((min(x1,x2)+15,y-29),s,font=small,fill='black')
for a,b,y,s in [(0,1,125,'TCP connect, LOGIN alice'),(1,0,180,'OK, END'),(2,1,235,'TCP connect, LOGIN bob'),(1,2,290,'OK, END'),(2,1,345,'LOGOUT'),(0,1,410,'SEND bob, текст'),(1,0,475,'OK или ERROR QUEUE_FULL'),(2,1,540,'TCP connect, LOGIN bob'),(1,2,605,'MESSAGE id, текст'),(2,1,670,'ACK id'),(1,2,735,'END'),(2,1,800,'LOGOUT')]:arrow(a,b,y,s)
im.save(p/'sequence.png')
page();heading('ВВЕДЕНИЕ')
para('Сетевое взаимодействие позволяет независимым процессам обмениваться данными. TCP обеспечивает надёжный упорядоченный поток байтов, однако границы сообщений и правила обработки отключений определяются прикладным протоколом.')
para('В работе разработан клиент-серверный чат с временным хранением сообщений и подтверждением их получения. Проверены порядок передачи, повторная выдача и ограничение длины очереди.')
heading('1 ИНДИВИДУАЛЬНОЕ ЗАДАНИЕ')
para('Вариант 4. Реализовать TCP-чат с одним сервером и не менее чем тремя клиентами. Сервер временно сохраняет до 20 непрочитанных сообщений на пользователя. Поддержать LOGIN, SEND, GET_PENDING и LOGOUT. При повторном подключении выдавать сообщения в порядке поступления. При заполнении очереди отклонять новые сообщения с уведомлением отправителя.')
para('Исследовать отправку 5 и 20 сообщений отключённому адресату; проверить порядок, отсутствие повторной выдачи подтверждённых сообщений и переполнение. STUDENT_ID = 27; порт 20000 + 27 = 20027; SEED = 3619, случайные данные в эксперименте не используются.')
heading('2 ЦЕЛЬ РАБОТЫ')
para('Изучить организацию сетевого взаимодействия процессов и получить навыки разработки клиентских и серверных приложений с использованием Windows Sockets.')
heading('3 ВЫПОЛНЕНИЕ ЗАДАНИЯ');heading('3.1 Теоретические сведения')
para('TCP предоставляет соединение и упорядоченную доставку байтов. Вызовы send и recv могут обработать только часть буфера; поэтому в программе они вызываются в цикле. Для выделения границ применяется длина кадра. Выбор TCP соответствует требованию варианта и упрощает сохранение порядка передачи.')
heading('3.2 Архитектура программы')
para('Программа запускается отдельными процессами в режиме server или client. Сервер принимает подключения и обслуживает каждое в отдельном потоке. Общие очереди и множество подключённых имён защищены мьютексом. Очередь каждого адресата реализована как FIFO и хранится в оперативной памяти до завершения сервера.')
para('Отправитель получает результат постановки в очередь. На LOGIN сервер автоматически передаёт ожидающие сообщения; во время сеанса клиент запрашивает GET_PENDING примерно раз в секунду и поддерживает ручной запрос. После каждого MESSAGE клиент отправляет ACK с идентификатором сообщения; сервер удаляет только подтверждённую головную запись. При обрыве до приёма ACK запись сохраняется. Повторная выдача возможна, если ACK не дошёл до сервера; доставка ровно один раз при сбоях не гарантируется.')
heading('3.3 Прикладной протокол')
para('Кадр содержит 4-байтовую беззнаковую длину в сетевом порядке и от 1 до 65536 байт полезной нагрузки UTF-8. Поля разделены табуляцией; первые поля содержат версию 1, тип и номер запроса. Имена ограничены 32 символами; текст сообщения не должен содержать табуляцию.')
para('Важный фрагмент реализации: отправка и приём повторяются до обработки всех байтов кадра; длина передаётся в сетевом порядке:')
source=(root/'LAB5'/'LAB5.cpp').read_text(encoding='utf-8')
frame_code=source.split('static bool transfer_all',1)[1].split('static bool send_frame',1)[0]
for line in ('static bool transfer_all'+frame_code.rstrip()).splitlines():
 q=para(line,indent=False)
 for run in q.runs: run.font.name='Courier New'; run.font.size=Pt(9)
para('Таблица 1 – Сообщения протокола',indent=False)
table(['Тип','Направление','Содержимое и реакция'],[['LOGIN','Клиент → сервер','Имя; OK и выдача очереди или ERROR'],['SEND','Клиент → сервер','Адресат и текст; OK или ERROR QUEUE_FULL'],['GET_PENDING','Клиент → сервер','Запрос выдачи ожидающей очереди'],['MESSAGE / ACK','Сервер ↔ клиент','Номер, отправитель, текст / подтверждение номера'],['END','Сервер → клиент','Очередь на момент выдачи пуста'],['LOGOUT','Клиент → сервер','Штатное завершение; OK'],['ERROR','Сервер → клиент','Причина отказа, включая неверную команду']])
heading('3.4 Диаграмма взаимодействия');para('На рисунке 1 показана работа двух клиентов. Полное редактируемое описание диаграммы приведено в файле experiments/sequence.mmd в формате Mermaid.')
image(p/'sequence.png',15);para('Рисунок 1 – Диаграмма последовательности обмена',center=True)
heading('3.5 Сетевые функции и обработка ошибок')
para('В Windows используются WSAStartup, select, socket, bind, listen, accept, connect, send, recv, setsockopt, shutdown, closesocket, WSAGetLastError и WSACleanup. Размер кадра проверяется перед выделением буфера; проверяются версия, тип, идентификатор запроса, обязательные поля и длина очереди. select ожидает начало следующего запроса без отключения при простое. Тайм-аут ограничивает ожидание при обмене кадрами и подтверждениями; неожиданное закрытие соединения завершает обработчик, а непрочитанные записи остаются в очереди.')
para('События сервера содержат время, роль, имя, адрес клиента, операцию и идентификатор запроса; события клиента выводятся в консоль. Ошибки создания сокета, bind/listen и connect выводятся с кодом и текстом Windows. Ошибки приёма и отправки завершают текущий сеанс. Команда stop завершает приём соединений, после чего сервер ожидает окончания потоков.')
heading('3.6 Методика экспериментов')
para('Проверка выполнена на macOS через 127.0.0.1:20027 с использованием POSIX-ветви программы; сервер работал отдельным процессом, а тестовый сценарий создавал три независимых TCP-соединения Alice, Bob и Carol. Штатные клиентские процессы дополнительно проверены отдельно. Версия Winsock для Windows в данной среде не запускалась. Bob отключался, Alice и Carol поочерёдно отправляли ему сообщения, затем Bob подключался повторно. Для каждой конфигурации выполнено три запуска с новым сервером. Дополнительная конфигурация из 21 сообщения проверяет переполнение.')
para('Измерено суммарное время запросов SEND от начала первого запроса до получения ответа на последний; оно включает обработку сервера и локальный обмен, но не подключение и последующую выдачу очереди. Использован time.perf_counter. Значение среднего времени запроса получено делением общего времени на число запросов. Дополнительные проверки: совпадение списка идентификаторов, отправителей и текстов с принятыми запросами SEND, пустой GET_PENDING после ACK и пустая очередь после ещё одного входа.')
heading('3.7 Результаты и анализ')
rows=list(csv.DictReader((p/'results.csv').open(encoding='utf-8')))
para('Таблица 2 – Время серии запросов SEND',indent=False)
t=[]
for n in (5,20,21):
 a=[float(r['send_total_ms']) for r in rows if int(r['attempted'])==n];t.append([n,*[f'{x:.3f}' for x in a],f'{statistics.mean(a):.3f}'])
table(['Запросов','1, мс','2, мс','3, мс','Среднее, мс'],t)
para('Таблица 3 – Результаты каждого повтора',indent=False)
table(['Отправлено','Принято','Отклонено','Выдано','Порядок / повтор'],[[5,5,0,5,'сохранён / нет'],[20,20,0,20,'сохранён / нет'],[21,20,1,20,'сохранён / нет']])
para('Для 5 и 20 запросов переполнения не возникло. В каждом запуске с 21 запросом сервер отклонил одно новое сообщение и уведомил отправителя. Подтверждённые сообщения не появились ни при повторном запросе, ни после нового подключения. Различия времени отражают условия локального запуска; результаты не являются измерением задержки в реальной сети.')
image(p/'time.png');para('Рисунок 2 – Среднее время серии запросов SEND',center=True)
image(p/'rejected.png');para('Рисунок 3 – Число отклонённых сообщений',center=True)
heading('3.8 Запуск программы')
para('Открыть LAB5.slnx в Visual Studio и собрать проект. Запустить сервер: LAB5.exe server --host 127.0.0.1 --port 20027. В отдельных консолях запустить клиентов с параметрами client --id alice, client --id bob и client --id carol. Доступны SEND bob текст, GET_PENDING и LOGOUT. Для завершения сервера ввести stop. Дополнительные параметры и команда воспроизведения эксперимента приведены в README.md.')
heading('ВЫВОД')
para('Разработан TCP-чат с временными очередями и подтверждением выдачи. Эксперименты подтвердили FIFO, отсутствие повторной выдачи полученных сервером подтверждений и отказ при превышении лимита 20 сообщений. Изучены функции сокетов, границы сообщений TCP и обработка отключения клиента.')
heading('СПИСОК ИСПОЛЬЗОВАННЫХ ИСТОЧНИКОВ')
para('1 Microsoft. Windows Sockets 2 [Электронный ресурс]. URL: https://learn.microsoft.com/windows/win32/winsock/windows-sockets-start-page-2 (дата обращения: 05.11.2026).',indent=False)
para('2 Методические указания к лабораторной работе «Элементы сетевого программирования».',indent=False)
page();heading('ПРИЛОЖЕНИЕ А ИСХОДНЫЕ ДАННЫЕ')
para('Полные данные приведены в experiments/results.csv. Объём включает кадры SEND и ответы с четырёхбайтовыми заголовками. Скорость рассчитана как объём этих кадров, делённый на длительность серии. RTT здесь представляет среднее время клиентской операции SEND, включая обмен по TCP и работу сервера.')
keys=['attempted','trial','send_responses','timeouts','send_frame_bytes','mean_send_rtt_ms','send_bytes_per_s']
table(['SEND','Прогон','Ответов','Тайм-аутов','Байт','Среднее, мс','Байт/с'],[[r[k] for k in keys] for r in rows])
page();heading('ПРИЛОЖЕНИЕ Б ИСХОДНИКИ ГРАФИЧЕСКИХ МАТЕРИАЛОВ')
para('Редактируемые файлы: experiments/plots.py, experiments/results.csv и experiments/sequence.mmd. Ниже приведено исходное описание диаграммы Mermaid.')
for line in (p/'sequence.mmd').read_text().splitlines():
 q=para(line,indent=False);q.alignment=WD_ALIGN_PARAGRAPH.LEFT
 for r in q.runs:r.font.name='Courier New';r.font.size=Pt(9)
para('Для воспроизведения графиков выполнить python experiments/plots.py; требуется Pillow. Полный исходный код программы и теста расположен в проекте. Исходники, результаты экспериментов и отчёт хранятся в локальном репозитории Git в ветке main. Для сдачи требуется отправить ветку в закрытый репозиторий, доступный преподавателю.')
d.save(root/'Отчёт_ЛР5_вариант4_итог.docx')
