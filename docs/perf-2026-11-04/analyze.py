import csv, statistics as st

ORDER = ["rg353m","rg353v","rk2023","rgb30a","rgb30b","rgb30c","rg351mpa","rg351mpb"]
CARTS = ["ping","Blocky_Boss_Battle","arcade-cube-land"]
SHORT = {"ping":"ping","Blocky_Boss_Battle":"Blocky","arcade-cube-land":"cubeland"}
MODEL = {"rg353m":"RG353M","rg353v":"RG353V","rk2023":"RK2023","rgb30a":"RGB30 #1",
         "rgb30b":"RGB30 #2","rgb30c":"RGB30 #3","rg351mpa":"RG351MP #1","rg351mpb":"RG351MP #2"}
SOC   = {"rg353m":"RK3566@2.0","rg353v":"RK3566@1.8","rk2023":"RK3566@2.0","rgb30a":"RK3566@2.0",
         "rgb30b":"RK3566@2.0","rgb30c":"RK3566@2.0","rg351mpa":"RK3326@1.5","rg351mpb":"RK3326@1.5"}

rows = [r for r in csv.reader(open("out/deploy/bench_results.csv", encoding="utf-8"))
        if r and r[0] == "RESULT"]
per = {}
for i, r in enumerate(rows):
    host = ORDER[i // 9]
    per.setdefault((host, r[2]), []).append(float(r[4]))

# rg353m / rg353v 用统一脚本后的重测值覆盖
retest = {
 ("rg353m","ping"):[364.0,364.7,361.0],
 ("rg353m","Blocky_Boss_Battle"):[250.3,249.1,248.9],
 ("rg353m","arcade-cube-land"):[163.2,163.7,163.6],
 ("rg353v","ping"):[374.5,376.6,373.1],
 ("rg353v","Blocky_Boss_Battle"):[255.9,255.5,256.2],
 ("rg353v","arcade-cube-land"):[165.5,166.1,165.7],
}
per.update(retest)

print("| 设备 | SoC | ping | Blocky | cubeland |")
print("|---|---|---|---|---|")
med = {}
for h in ORDER:
    vals = []
    for c in CARTS:
        m = st.median(per[(h, c)])
        med[(h, c)] = m
        vals.append(f"{m:.0f}")
    print(f"| {MODEL[h]} | {SOC[h]} | " + " | ".join(vals) + " |")

print()
for soc in ("RK3566@2.0", "RK3326@1.5"):
    hs = [h for h in ORDER if SOC[h] == soc]
    cs = [med[(h,"arcade-cube-land")] for h in hs]
    print(f"{soc}: cubeland 中位 {st.median(cs):.0f} fps，极差 {min(cs):.0f}–{max(cs):.0f}（{(max(cs)/min(cs)-1)*100:.0f}% 离散）")

ratio = st.median([med[(h,"arcade-cube-land")] for h in ORDER if SOC[h]=="RK3566@2.0"]) / \
        st.median([med[(h,"arcade-cube-land")] for h in ORDER if SOC[h]=="RK3326@1.5"])
print(f"RK3566@2.0 / RK3326@1.5 = {ratio:.2f}×")
print()
print("60fps 目标下的余量（cubeland）：")
for h in ORDER:
    print(f"  {MODEL[h]:12s} {med[(h,'arcade-cube-land')]/60:.2f}×")
