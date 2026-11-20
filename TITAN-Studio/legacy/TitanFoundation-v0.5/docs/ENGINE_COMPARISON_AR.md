# TITAN Engine — مقارنة هندسية قائمة على المصدر (5 November 2026)

**حدود المراجعة:** راجعنا مسارات مستودعات المصدر العامة الموثقة ومصادر رسمية. لم نُجرِ benchmark متكافئًا لكل هذه المحركات، ولم نراجع كل سطر فيها. المستودع الذي قدمه صاحب المشروع `AlexMercer-MA/UnrealEngine-4.27` يحتوي على `Engine/Source/{Runtime,Editor,Developer,Programs,ThirdParty}`، وملف `Engine/Build/Build.version` يعرّف الإصدار 4.27.2. لا تتضمن هذه الحزمة أي نسخة من كود Epic أو المحركات الأخرى.

| المحرك | لغة / وحدات كود جرى فحص هيكلها | أفضل فكرة لـTITAN | التنفيذ العملي في حزمة v0.2 | شروط استخدام المصدر |
|---|---|---|---|---|
| Unreal Engine 4.27 | C++، `Engine/Source/Runtime/Engine`, `.../Renderer`, `.../CoreUObject` | نظام العالم، نمط UObject، UBT، قابلية توسيع Gameplay Framework | Adapter لـUE4.27؛ عدم نقل ملفات Epic | EULA؛ وصول/توزيع منفصلين |
| Unreal Engine 5.8 | C++، توثيق Nanite/Lumen/World Partition/Insights | Virtualized geometry، dynamic GI، world partition، profiling | مخطط R&D لاحق، **لا** توجد Nanite/Lumen في المشروع | EULA/اتفاقية خاصة |
| Unity | C++/C#، `UnityCsReference` public C# layer | واجهات editor وDOTS/workflow والتصدير | دراسة تصميم، بدون نسخ | Reference only للنسخة العامة؛ Source Adapt ترخيص تجاري |
| Godot | C++، `scene/main/scene_tree.cpp` و`servers/rendering` | فصل scene عن server عن platform/driver | modular interface design | MIT؛ راجع تبعيات الطرف الثالث |
| O3DE | C++، `Gems/Atom/{RHI,RPI,Feature}`, Gems | Frame graph / Pass-based RHI وذاكرة transient | `TitanFrameGraph` compile+alias plan | Apache 2.0؛ راجع الوحدات التابعة |
| Bevy | Rust، `crates/bevy_ecs/src/schedule`, ECS API | systems scheduling، data-access hazards | `TitanTaskGraph` portable | MIT أو Apache 2.0 حسب الملف |
| Wicked Engine | C++، `WickedEngine/` وshaders | GPU-first rendering، shaders، back-end abstraction | اتجاه للـRenderer، ليس Renderer فعليًا | MIT؛ لاحظ أصول/مكتبات أخرى |
| Stride | C#، `sources/engine/Stride.Rendering`, `Stride.Graphics` | Render pipeline وmaterial/editor tooling | مخطط واجهات رسم/موارد | MIT غالبًا مع استثناءات ملفات |
| CRYENGINE | C++، وثائق المصدر وبيئة Crytek | terrain، foliage، complex materials | مسار بحث للـvegetation | الترخيص الخاص Crytek، ليس MIT |
| Flax | C++/C#، مصدر المحرك + محرر | integrated workflow وcustom engine tooling | مسار بحث لأدوات المحرر | EULA وحقوق نشر محددة |
| Panda3D | C++ core / Python binding | binding مستقل وتكامل Python | API tools roadmap | BSD مع third parties |

## المصدر البرمجي المقروء / التوثيق

- Unreal source نسخة المستخدم: https://github.com/AlexMercer-MA/UnrealEngine-4.27 (بيانات البناء: `Engine/Build/Build.version`).
- UE5: https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine
- UE5 Lumen: https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine
- Godot scene source: https://github.com/godotengine/godot/blob/master/scene/main/scene_tree.cpp
- Godot architecture: https://docs.godotengine.org/en/stable/engine_details/architecture/godot_architecture_diagram.html
- O3DE Atom frame scheduler: https://docs.o3de.org/docs/atom-guide/dev-guide/rhi/frame-scheduler/
- O3DE Atom rendering source: https://github.com/o3de/o3de/tree/development/Gems/Atom
- Bevy ECS: https://github.com/bevyengine/bevy/tree/main/crates/bevy_ecs/src/schedule
- Bevy ECS change detection: https://github.com/bevyengine/bevy/blob/main/crates/bevy_ecs/README.md
- Wicked Engine: https://github.com/turanszkij/WickedEngine/tree/master/WickedEngine
- Stride renderer: https://github.com/stride3d/stride/tree/master/sources/engine/Stride.Rendering
- Unity source: https://github.com/Unity-Technologies/UnityCsReference
- CryEngine licensing: https://www.cryengine.com/support/view/licensing
- Flax source and license: https://github.com/FlaxEngine/FlaxEngine and https://flaxengine.com/licensing/
- Panda3D: https://www.panda3d.org/

## اختيار البنية للشركة

**Core:** C++14 portable logic + UE4.27 integration adapter (الحالي)؛ على المدى الأبعد TITAN Runtime مستقل وBackend مستقل عن Unreal. لا نربط كل جزء محوري بـUnreal UObject حتى تبقى مرونة الانتقال ممكنة.

**World/Memory:** منهج World Partition + resource budgets لكن مع إقرارات I/O صريحة؛ لذلك لا نفرغ الذاكرة عند مجرد إرسال طلب Unload.

**Rendering:** RHI abstraction مثل Godot/O3DE؛ Pass graph compiler من TITAN يحدد hazards/lifetimes؛ بناء renderer فعلي عبر Metal/Vulkan/DX12 مشروع منفصل يحتاج فريق GPU.

**Tasking:** Bevy data-access / change detection مصدر أفكار لتحديد مجموعات الوظائف المتوازية؛ الـTaskGraph الحالي C++14 portable ويستعمل read/write resource sets بشكل مبسط.

**Quality:** UE5/CryEngine/Flax أنظمة الجرافيكس معقدة؛ لا يمكن ادعاء تكافؤ من دون benchmarks ومشاهد قاسية وإجراءات قياس وفحص مرئي.

## غير موجود حاليًا

- **ليس** لدى TITAN v0.2 RHI فعلي ولا Nanite ولا Lumen ولا shader compiler ولا rasterizer.
- الـFrameGraph ينتج خطة تنفيذ وpool slots فقط؛ لا ينفذ GPU passes.
- الـWorldScheduler يخرج طلبات load/unload؛ لا يتصل بملفات الأصول والـPak/IoStore تلقائيًا.
- الـTaskGraph ينسق وظائف native موصوفة بتبعيات، ولا يسمح باستدعاء Unreal UObjects من خيوط أخرى.
- لم تتم تجربة البناء داخل UE4.27 أو Metal بعد؛ اختبار portable C++ فقط.
