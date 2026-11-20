# مقارنة معمارية لأبرز محركات الألعاب — نوفمبر 2026

**هذه مقارنة هندسية وظيفية وليست ترتيبًا عالميًا للأداء.** تم فحص ترتيب مجلدات المصدر المفتوح للمشاريع المذكورة حيث أمكن؛ تمت مراجعة التوثيق العام للمحركات مغلقة المصدر فقط. لا يجوز استنتاج أداء FPS أو تفوق مطلق دون Benchmark موحد.

| المحرك | مصدر المعلومة | الخصائص/التصميم المرجعي | قرار TITAN |
|---|---|---|---|
| Unreal Engine 5.8 | توثيق Epic (UE5)، ومستودع المستخدم UE4.27.2 (UE4) | World Partition، PCG، Lumen، MegaLights، Gameplay Framework | تطوير Streaming فعلي على UE4.27 وPCG/Rendering مستقبلًا، مع عدم الادعاء بوجود ميزات UE5 في UE4 |
| Unity DOTS | توثيق Unity ECS | Data-oriented entities وqueries وjob dependencies | ECS مخزن packed مع joins ونظام مهام مستقل |
| Bevy 0.20 | مصدر `crates/bevy_ecs/src/schedule` وإعلان 0.20 | Resource-aware scheduling، ECS، render improvements | تحسين إدارة اعتماديات Task Graph والـECS؛ حاليًا لا يوجد ECS متعدد الخيوط |
| O3DE Atom | مصدر `Gems/Atom/RHI` و`RPI` والتوثيق | Render Hardware Interface وFrame Scheduler | Frame Graph compiler موجود؛ مطلوب backend GPU مستقل |
| Godot | المصدر `servers/`, `scene/` والتوثيق | Scene/Server/Platform separation | فصل TITAN data/simulation والـrender candidate builder عن UE Actor layer |
| Wicked Engine | مجلد `WickedEngine` ومكتبات graphics | Low-level graphics pipeline وasset/render routines | CPU visibility + batching؛ GPU backend مرحلة لاحقة |
| Stride 4.4 | المصدر `Stride.Rendering` وتوثيق 4.4 | Graphics API / shader compiler / asset pipeline | تصميم Renderer وAsset pipeline مستقلين في الإصدارات التالية |
| Flax Engine | التوثيق العام + المصدر المتاح بترخيص Flax | C++/C# engine/editor وworkflow | دراسة تواصل editor/runtime، دون نسخ كود Flax |
| CryEngine | توثيق SVOGI | Voxel-based global illumination | تقييم GI بعد وجود GPU renderer |
| Frostbite | ميزات منشورة علنًا فقط | Large-scale rendering/toolchain للألعاب الضخمة | مرجع طموح لا يوجد لدينا فحص كود المصدر |
| Decima | ميزات منشورة علنًا فقط | Massive streaming/open-world animation | مرجع متطلبات؛ لا ندعي فحص المصدر |
| RAGE / RE Engine / id Tech / Snowdrop | مزايا منشورة علنًا فقط | Renderer/streaming/animation حسب المنتج | Benchmarks مستقبلية فقط، دون مقارنة معمارية من كودها غير العام |

## لماذا هذه الأولويات؟

1. **Generational ECS:** كيان تم حذفه ثم أعيد استخدام index له لا ينبغي أن يقبل handle قديم؛ تم تطبيق ذلك واختباره.
2. **Packed pools:** تخزين مكونات متجاورة بدل كائن كبير مع مئات الحقول؛ يتم تغيير مخزن الكيان عبر swap-remove.
3. **Fixed-step:** فصل معدل المحاكاة عن معدل الـFPS وتحجيم التراكم حتى لا يحدث spiral-of-death.
4. **Visibility + LOD:** منع إرسال مرشحي رسم خارج مجال الرؤية واختيار detail حسب footprint على الشاشة؛ لا يوجد occlusion أو mesh shaders حتى الآن.
5. **UE4 plug-in bridge:** تكامل تدريجي قابل للعزل والاختبار بدل نسخ عشرات الملايين من سطور الأكواد.

## روابط التحقق الأصلية

- Unreal 5.8: https://www.unrealengine.com/news/unreal-engine-5-8-is-now-available
- Epic UE5 docs: https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-8-release-notes
- Unity ECS: https://unity.com/ecs
- Bevy 0.20: https://bevy.org/news/bevy-0-20/
- Bevy scheduling source: https://github.com/bevyengine/bevy/tree/main/crates/bevy_ecs/src/schedule
- O3DE Atom RHI: https://www.docs.o3de.org/docs/atom-guide/dev-guide/rhi/
- O3DE Atom source: https://github.com/o3de/o3de/tree/development/Gems/Atom
- Godot architecture: https://docs.godotengine.org/en/stable/engine_details/architecture/godot_architecture_diagram.html
- Godot server source: https://github.com/godotengine/godot/tree/master/servers
- Wicked Engine: https://github.com/turanszkij/WickedEngine
- Stride 4.4: https://stride-website.azurewebsites.net/blog/announcing-stride-4-4/
- Stride Rendering: https://github.com/stride3d/stride/tree/master/sources/engine/Stride.Rendering
- CryEngine GI: https://www.cryengine.com/docs/static/engines/cryengine-5/categories/23756816/pages/25535599
- Flax licensing: https://flaxengine.com/licensing/
