"""Independent algebraic verification of exported controller motion and residence."""
import argparse
import csv
import json
import math
from pathlib import Path


def close(a, b, label):
    if not math.isclose(a, b, rel_tol=2e-12, abs_tol=2e-14):
        raise AssertionError(f"{label}: {a} != {b}")


def zone_at(space, environment, x, y):
    matches = []
    for zone in environment["zones"]:
        if zone["shape"] == 1:
            inside = math.hypot(x - zone["x"], y - zone["y"]) <= zone["width"]
        else:
            right, bottom = zone["x"] + zone["width"], zone["y"] + zone["height"]
            inside = (x >= zone["x"] and y >= zone["y"]
                      and (x < right or x == right == space["width"])
                      and (y < bottom or y == bottom == space["height"]))
        if inside:
            matches.append((-zone["priority"], int(zone["id"])))
    return min(matches)[1] if matches else 0


def verify(folder):
    meta = json.loads((folder / "result_animat_experiment.json").read_text(encoding="utf-8"))
    analysis = json.loads((folder / "result_animat_analysis.json").read_text())
    config, environments = meta["config"], meta["environments"]
    controller, space = config["animat"], config["space"]
    rows = list(csv.DictReader((folder / "result_animat.csv").open()))
    start, stop = analysis["available_from"], analysis["available_to"]
    previous = None
    last = (0., 0.)
    path = 0.
    dwell = {}
    ticks = 0
    for index, row in enumerate(rows):
        t, x, y = (float(row[key]) for key in ("time_s", "x", "y"))
        kind = int(row["kind"])
        if previous:
            assert t >= previous[0], "time went backwards"
        assert 0 <= x <= space["width"] and 0 <= y <= space["height"]
        environment = environments[int(row["environment"])]["value"]
        assert int(row["zone_id"]) == zone_at(space, environment, x, y)
        if kind == 1:
            ticks += 1
            vx, vy = float(row["vx"]), float(row["vy"])
            dx = controller["gain"] * vx * space["width"]
            dy = controller["gain"] * vy * space["height"]
            active = math.hypot(vx, vy) > controller["zero_threshold"]
            if not active:
                dx, dy = last if controller["silence"] == 1 else (0., 0.)
            length = math.hypot(dx, dy)
            if length > controller["max_displacement"] and length:
                factor = controller["max_displacement"] / length
                dx, dy = dx * factor, dy * factor
            close(dx, float(row["requested_dx"]), "requested dx")
            close(dy, float(row["requested_dy"]), "requested dy")
            nx, ny = previous[1] + dx, previous[2] + dy
            if controller["boundary"] == 0:
                nx, ny = min(space["width"], max(0, nx)), min(space["height"], max(0, ny))
            elif not (0 <= nx <= space["width"] and 0 <= ny <= space["height"]):
                nx, ny = previous[1:]
            close(nx, x, "position x")
            close(ny, y, "position y")
            if active:
                last = (dx, dy)
            if start <= t < stop:
                path += math.hypot(x - previous[1], y - previous[2])
        elif kind == 2:
            last = (0., 0.)
        elif kind == 4 and int(row["stage_index_0based"]) >= 0:
            stage = config["stages"][int(row["stage_index_0based"])]["animat"]
            if stage["home"] or stage.get("position", False):
                last = (0., 0.)
        next_time = float(rows[index + 1]["time_s"]) if index + 1 < len(rows) else meta["time"]
        duration = max(0., min(next_time, stop) - max(t, start))
        key = (int(row["stage_run"]), int(row["zone_id"]))
        dwell[key] = dwell.get(key, 0.) + duration
        previous = (t, x, y)
    close(path, analysis["path"], "integrated path")
    for row in csv.DictReader((folder / "result_animat_zones.csv").open()):
        key = (int(row["stage_run"]), int(row["zone_id"]))
        close(float(row["residence_s"]), dwell.get(key, 0.), "zone residence")
    close(sum(dwell.values()), stop - start, "total residence")
    return {"name": folder.name, "ticks": ticks, "path": path, "residence": sum(dwell.values())}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    report = json.loads((args.directory / "report.json").read_text(encoding="utf-8"))
    results = [verify(args.directory / row["name"]) for row in report["runs"]]
    config = json.loads((args.directory / "spatial_learning.json").read_text(encoding="utf-8"))
    inhibitory = [i for i, synapse in enumerate(config["synapses"])
                  if config["neurons"][synapse["source"]]["inhibitory"]]
    runs = {row["name"]: row for row in report["runs"]}
    common = runs["common"]["summary"]["final_synapse_weights"]
    weight_checks = {}
    for name, row in runs.items():
        if name.startswith(("branch_", "learning_")):
            weights = row["summary"]["final_synapse_weights"]
            assert len(weights) == len(common)
            assert all(weights[i] == common[i] for i in inhibitory), "inhibitory weight changed"
            changes = sum(a != b for a, b in zip(weights, common))
            if name.startswith("learning_frozen"):
                assert changes == 0, "frozen weights changed"
            weight_checks[name] = {"changed": changes, "unchanged_inhibitory": len(inhibitory)}
    snapshots = {}
    for row in csv.DictReader((args.directory / "spatial_learning_1/result_animat_stage_states.csv").open()):
        if row["kind"] == "weight_pre_post_slow":
            snapshots.setdefault((int(row["stage_run"]), int(row["exit"])), {})[
                int(row["index"])] = float(row["value"])
    stage_changes = {}
    for stage in range(1, 5):
        before, after = snapshots[(stage, 0)], snapshots[(stage, 1)]
        assert len(before) == len(after) == 4 * len(config["synapses"])
        changes = sum(before[i * 4] != after[i * 4] for i in range(len(config["synapses"])))
        assert all(before[i * 4] == after[i * 4] for i in inhibitory)
        if stage in (1, 3):
            assert changes == 0, "frozen stage changed weights"
        stage_changes[str(stage)] = changes
    output = {"motion_and_residence": results, "weights": weight_checks, "stage_weight_changes": stage_changes}
    (args.directory / "independent_check.json").write_text(json.dumps(output, indent=2), encoding="utf-8")
    print(f"PASS: independent motion and residence verification in {len(results)} runs, "
          f"{sum(r['ticks'] for r in results)} controller updates")
    print(f"PASS: frozen weights and all {len(inhibitory)} inhibitory weights unchanged; "
          f"stage weight changes {stage_changes}")


if __name__ == "__main__":
    main()
