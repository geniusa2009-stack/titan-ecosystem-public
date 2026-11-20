# TITAN Engine Architecture v0.4 — كود أصلي فوق UE4.27.2

نسخة مطوّرة من `TITAN-UE427-Repo-Integration-v0.3`، مبنية لمستودع `AlexMercer-MA/UnrealEngine-4.27` الذي يعرّف نفسه كـUE4.27.2. **لا تتضمن هذه الحزمة كود Epic أو كود أي محرك آخر**. كل الإضافات الجديدة مكتوبة هنا من البداية، وتقوم على مبادئ هندسية عامة ومصادر منشورة. احتفظ بإثبات التراخيص قبل إعادة توزيع أي منتج يحتوي على كود UE.

## منفذ فعلًا في v0.4

- `TitanDataCore`: نظام `EntityWorld` مع معرفات 32-bit index + 32-bit generation، منع استخدام المعرفات القديمة، مخازن packed/sparse components، حذف swap-remove، إلحاق مكونات مختلفة، تكرار مكونين مع اختيار الـpool الأصغر، وDeferredCommands لمنع التغييرات الهيكلية أثناء التكرار.
- `TitanFixedClock`: خطوة ثابتة محددة مع حد أقصى للحاق بالتأخير، وفصل الجزء الباقي لاستخدامه في interpolation مستقبلًا.
- `TitanVisibility`: CPU frustum culling باستخدام bounding spheres، اختيار 3 مستويات LOD من الحجم الظاهر على الشاشة، وتجميع draw candidates حسب الـmesh والـmaterial والـLOD.
- `UTitanSimulationComponent`: طبقة UE4.27 اختيارية، تضيف وتحرك كيانات منطقية وتكشفها للـBlueprints بدون إنشاء Actors؛ **ليست Render bridge ولا Physics Solver**.
- جميع أنظمة v0.3 السابقة: Native Level Streaming Bridge، World Scheduler، Frame Graph compiler، Task Graph، Adaptive Quality.

## بناء واختبار خارج Unreal (macOS/Linux)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
./build/titan_ecs_benchmark
python3 -m unittest discover -s tests -p 'test_install_*.py' -v
```

المقاييس الناتجة من الـBenchmark على جهازك فقط؛ لا تقارنها بنتائج Unreal/Unity من دون نفس المشهد والظروف والعتاد.

## الدمج في نسخة Unreal القديمة

1. أنشئ Git branch في **نسختك المحلية** من UE4.27 قبل التغيير.
2. لو سبق تثبيت `TitanFoundation` الإصدار v0.3، أعد تسميته/انسخه خارج `Engine/Plugins/Runtime/` أولًا بعد حفظ أي تعديلات. المثبت يرفض الكتابة فوق أي نسخة موجودة.
3. فك الملف ZIP في مجلد مستقل، ثم شغّل:

```bash
python3 scripts/install_into_ue427.py --engine /FULL/PATH/TO/UnrealEngine-4.27
python3 scripts/install_into_ue427.py --engine /FULL/PATH/TO/UnrealEngine-4.27 --apply
```

4. افتح مشروع UE4.27 وأفعّل Plugin **TITAN Foundation**؛ أضف `Titan Simulation Component` إلى Actor واستخدم `Spawn Logical Entity` و`Get Logical Position` من Blueprint.
5. عند تشغيل UE، الكيانات الجديدة منطقية فقط: لن تظهر في المشهد حتى توصلها أنت بـStatic Mesh Actors أو InstancedStaticMesh أو Render Backend مستقل.

**التحقق الحالي:** كود C++ المستقل تم تجميعه واختباره على Linux باستخدام GCC وClang مع ASan/UBSan، لكن **لم يُبنَ Plugin داخل Unreal ولا على Mac M3**. راجع `docs/ENGINE_COMPARISON_2026_AR.md` و`docs/ORIGINAL_CODE_MAP.md` و`docs/VALIDATION_V04.md`.


## إضافات v0.5
راجع README_AR_V05.md و docs/V05_SOURCE_ANALYSIS_AR.md لمعلومات تفعيل إحصاءات الـStreaming.
