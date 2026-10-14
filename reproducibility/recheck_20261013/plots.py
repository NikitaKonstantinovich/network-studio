"""Export static scientific comparison figures from measured laboratory data."""
import argparse, os, statistics
from pathlib import Path
import numpy as np
from replay import read

def main(out,only_lab1=False):
    here=Path(__file__).resolve().parent;dest=here/'figures';dest.mkdir(exist_ok=True)
    os.environ.setdefault('MPLCONFIGDIR',str(out/'matplotlib_cache'))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':11,'axes.spines.top':False,'axes.spines.right':False})
    a=read(out/'analysis.json');ind=read(out/'independent/results.json');colors=['#8795a5','#118779']
    def finish(fig,name):
        fig.savefig(dest/(name+'.png'),dpi=160,bbox_inches='tight');fig.savefig(dest/(name+'.svg'),bbox_inches='tight');plt.close(fig)
    fig,ax=plt.subplots(figsize=(9,4.4),layout='constrained')
    names=['02_single_pulse','03_autonomous','04_periodic_02hz','04_periodic_10hz','04_periodic_20hz','05_dc_4']
    labels=['Один импульс','Автоколебания','2 Гц','10 Гц','20 Гц','Постоянный I=4'];x=np.arange(len(names))
    for shift,color,key,label in [(-.18,colors[0],'archive_25us','Архив, 25 мкс'),(.18,colors[1],'current_25us','Теперь, 25 мкс')]:
        ax.bar(x+shift,[ind['lab1'][n][key]['max_event_error_ms'] for n in names],.36,label=label,color=color)
    ax.set_yscale('log');ax.set_xticks(x,labels,rotation=15);ax.set_ylabel('Максимальная ошибка времени спайка, мс')
    ax.set_title('ЛР1: ошибка относительно независимого DOP853');ax.legend();ax.grid(axis='y',alpha=.2)
    finish(fig,'lab1_accuracy')
    if only_lab1:return
    fig,axs=plt.subplots(1,2,figsize=(10,4.3),layout='constrained')
    for ax,key,title in zip(axs,['mean_rate_hz','burst_rate_hz'],['Средняя частота спайков','Частота популяционных пачек']):
        for shift,color,which,label in [(-.18,colors[0],'old','Архив'),(.18,colors[1],'new','Теперь')]:
            sets=[[a['populations'][f'k{k}_seed{s}'][which][key] for s in (11,42,101)] for k in (15,30)]
            ax.bar(np.arange(2)+shift,[statistics.mean(v) for v in sets],.36,yerr=[statistics.stdev(v) for v in sets],capsize=4,color=color,label=label)
        ax.set_xticks([0,1],['15 ± 2 входа','30 ± 3 входа']);ax.set_ylabel('Гц');ax.set_title(title);ax.grid(axis='y',alpha=.2)
    axs[0].legend();fig.suptitle('ЛР2: 500 нейронов, окно 5–30 с, три seed (среднее ± SD)')
    finish(fig,'lab2_populations')
    fig,ax=plt.subplots(figsize=(9,4.5),layout='constrained');fs=[.1,.5,1,5,10,25,50,100]
    for which,color,label,style in [('old',colors[0],'Studio: архив','o--'),('new',colors[1],'Studio: теперь','s-')]:
        ax.plot(fs,[a['lab3'][f'f{f:g}'.replace('.','p')][which]['peak_mean'] for f in fs],style,color=color,label=label)
    ax.plot(fs,[14.5,14.5,14.5,17.5,15.5,10,6.75,6.75],'^:',color='#bc6939',label='NeuroNet: оценка по изображениям')
    ax.set_xscale('log');ax.set_xticks(fs,[str(f) for f in fs]);ax.set_xlabel('Частота внешних стимулов, Гц');ax.set_ylabel('Пик Output от нуля, у. е.')
    ax.set_title('ЛР3: исходный стимул 30 × 3 мс');ax.legend();ax.grid(alpha=.2)
    finish(fig,'lab3_frequency')
    names=sorted(n for n,r in a['lab4'].items() if r['group']=='lab4_updated' and n.endswith('_dt25us') and not n.startswith('width1'))
    labels=[n.removesuffix('_dt25us').replace('pre_post_pre','пре–пост–пре').replace('post_pre_post','пост–пре–пост').replace('pre_post','пре–пост').replace('post_pre','пост–пре').replace('triplet','триплет').replace('pair','пары') for n in names]
    changes=[100*(a['lab4'][n]['new_weights'][0]/a['lab4'][n]['old_weights'][0]-1) for n in names]
    fig,ax=plt.subplots(figsize=(10,6),layout='constrained');y=np.arange(len(names))
    ax.barh(y,changes,color=['#118779' if x>=0 else '#bc6939' for x in changes]);ax.set_yticks(y,labels,fontsize=9);ax.invert_yaxis()
    ax.axvline(0,color='#495766',lw=.7);ax.set_xlabel('Изменение конечного веса относительно прежнего Studio, %')
    ax.set_title('ЛР4: те же 14 протоколов по 100 с, шаг 25 мкс');ax.grid(axis='x',alpha=.2)
    finish(fig,'lab4_weight_changes')

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path);ap.add_argument('--only-lab1',action='store_true')
    args=ap.parse_args();main(args.output.resolve(),args.only_lab1)
