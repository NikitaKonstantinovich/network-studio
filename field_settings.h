#pragma once
namespace studio {
enum class FieldPolarity { All, Excitatory, Inhibitory };
enum class FieldModel { All, ConductanceSTP, ExponentialCurrent, TsodyksMarkram };
// Shared field settings. Controller persists only grid/filter/event-window fields;
// camera, visibility and arrow scale remain presentation-only.
struct TopologyViewState {
    bool field=false, edges=true, grid=true, all_edges=false, current=true, auto_scale=false;
    int columns=16, rows=12;
    double scale=.04, zoom=1, pan_x=0, pan_y=0;
    FieldPolarity polarity=FieldPolarity::All;
    FieldModel model=FieldModel::All;
    bool activity=false;
    int activity_mode=0;
    double activity_window=.05,response_min=0,response_max=.02,activity_scale=.04;
    FieldPolarity activity_polarity=FieldPolarity::All;
    FieldModel activity_model=FieldModel::All;
};
}
