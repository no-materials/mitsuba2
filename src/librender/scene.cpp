#include <mitsuba/core/properties.h>
#include <mitsuba/core/plugin.h>
#include <mitsuba/render/bsdf.h>
#include <mitsuba/render/medium.h>
#include <mitsuba/render/scene.h>
#include <mitsuba/render/kdtree.h>
#include <mitsuba/render/integrator.h>

#if defined(MTS_ENABLE_EMBREE)
#  include "scene_embree.inl"
#else
#  include "scene_native.inl"
#endif

#if defined(MTS_ENABLE_CUDA)
#  include "scene_optix.inl"
#endif

NAMESPACE_BEGIN(mitsuba)

MTS_VARIANT Scene<Float, Spectrum>::Scene(const Properties &props) {
    for (auto &kv : props.objects()) {
        m_children.push_back(kv.second.get());

        Shape *shape           = dynamic_cast<Shape *>(kv.second.get());
        Emitter *emitter       = dynamic_cast<Emitter *>(kv.second.get());
        Sensor *sensor         = dynamic_cast<Sensor *>(kv.second.get());
        Integrator *integrator = dynamic_cast<Integrator *>(kv.second.get());

        if (shape) {
            if (shape->is_emitter())
                m_emitters.push_back(shape->emitter());
            if (shape->is_sensor())
                m_sensors.push_back(shape->sensor());
            if (shape->is_shapegroup()) {
                m_shapegroups.push_back((ShapeGroup*)shape);
            } else {
                m_bbox.expand(shape->bbox());
                m_shapes.push_back(shape);
            }
        } else if (emitter) {
            // Surface emitters will be added to the list when attached to a shape
            if (!has_flag(emitter->flags(), EmitterFlags::Surface))
                m_emitters.push_back(emitter);

            if (emitter->is_environment()) {
                if (m_environment)
                    Throw("Only one environment emitter can be specified per scene.");
                m_environment = emitter;
            }
        } else if (sensor) {
            m_sensors.push_back(sensor);
        } else if (integrator) {
            if (m_integrator)
                Throw("Only one integrator can be specified per scene.");
            m_integrator = integrator;
        }
    }

    if (m_sensors.empty()) {
        Log(Warn, "No sensors found! Instantiating a perspective camera..");
        Properties sensor_props("perspective");
        sensor_props.set_float("fov", 45.0f);

        /* Create a perspective camera with a 45 deg. field of view
           and positioned so that it can see the entire scene */
        if (m_bbox.valid()) {
            ScalarPoint3f center = m_bbox.center();
            ScalarVector3f extents = m_bbox.extents();

            ScalarFloat distance =
                ek::hmax(extents) / (2.f * ek::tan(45.f * .5f * ek::Pi<ScalarFloat> / 180.f));

            sensor_props.set_float("far_clip", ek::hmax(extents) * 5 + distance);
            sensor_props.set_float("near_clip", distance / 100);

            sensor_props.set_float("focus_distance", distance + extents.z() / 2);
            sensor_props.set_transform(
                "to_world", ScalarTransform4f::translate(ScalarVector3f(
                                center.x(), center.y(), m_bbox.min.z() - distance)));
        }

        m_sensors.push_back(
            PluginManager::instance()->create_object<Sensor>(sensor_props));
    }

    if (!m_integrator) {
        Log(Warn, "No integrator found! Instantiating a path tracer..");
        m_integrator = PluginManager::instance()->
            create_object<Integrator>(Properties("path"));
    }

    if constexpr (ek::is_cuda_array_v<Float>)
        accel_init_gpu(props);
    else
        accel_init_cpu(props);

    if (!m_emitters.empty()) {
        // Create emitters' shapes (environment luminaires)
        for (Emitter *emitter: m_emitters)
            emitter->set_scene(this);

        // For cuda_* modes, convert the emitters pointers to enoki registry ids
        if constexpr (ek::is_jit_array_v<Float>) {
            std::vector<uint32_t> tmp(m_emitters.size());
            for (uint32_t i = 0; i < m_emitters.size(); i++)
                tmp[i] = jitc_registry_get_id(m_emitters[i]);
            m_emitters_ptr = ek::load_unaligned<EmitterPtr>(tmp.data(), tmp.size());
        } else {
            m_emitters_ptr = ek::load_unaligned<DynamicBuffer<EmitterPtr>>(m_emitters.data(), m_emitters.size());
        }
    }

    m_shapes_grad_enabled = false;
}

MTS_VARIANT Scene<Float, Spectrum>::~Scene() {
    if constexpr (ek::is_cuda_array_v<Float>)
        accel_release_gpu();
    else
        accel_release_cpu();
}

MTS_VARIANT typename Scene<Float, Spectrum>::SurfaceInteraction3f
Scene<Float, Spectrum>::ray_intersect(const Ray3f &ray, Mask active) const {
    MTS_MASKED_FUNCTION(ProfilerPhase::RayIntersect, active);

    if constexpr (ek::is_cuda_array_v<Float>)
        return ray_intersect_gpu(ray, HitComputeFlags::All, active);
    else
        return ray_intersect_cpu(ray, HitComputeFlags::All, active);
}

MTS_VARIANT typename Scene<Float, Spectrum>::SurfaceInteraction3f
Scene<Float, Spectrum>::ray_intersect(const Ray3f &ray, HitComputeFlags flags, Mask active) const {
    MTS_MASKED_FUNCTION(ProfilerPhase::RayIntersect, active);

    if constexpr (ek::is_cuda_array_v<Float>)
        return ray_intersect_gpu(ray, flags, active);
    else
        return ray_intersect_cpu(ray, flags, active);
}

MTS_VARIANT typename Scene<Float, Spectrum>::PreliminaryIntersection3f
Scene<Float, Spectrum>::ray_intersect_preliminary(const Ray3f &ray, Mask active) const {
    if constexpr (ek::is_cuda_array_v<Float>)
        return ray_intersect_preliminary_gpu(ray, active);
    else
        return ray_intersect_preliminary_cpu(ray, active);
}

MTS_VARIANT typename Scene<Float, Spectrum>::SurfaceInteraction3f
Scene<Float, Spectrum>::ray_intersect_naive(const Ray3f &ray, Mask active) const {
    MTS_MASKED_FUNCTION(ProfilerPhase::RayIntersect, active);

#if !defined(MTS_ENABLE_EMBREE)
    if constexpr (!ek::is_cuda_array_v<Float>)
        return ray_intersect_naive_cpu(ray, active);
#endif
    ENOKI_MARK_USED(ray);
    ENOKI_MARK_USED(active);
    NotImplementedError("ray_intersect_naive");
}

MTS_VARIANT typename Scene<Float, Spectrum>::Mask
Scene<Float, Spectrum>::ray_test(const Ray3f &ray, Mask active) const {
    MTS_MASKED_FUNCTION(ProfilerPhase::RayTest, active);

    if constexpr (ek::is_cuda_array_v<Float>)
        return ray_test_gpu(ray, active);
    else
        return ray_test_cpu(ray, active);
}

MTS_VARIANT std::tuple<typename Scene<Float, Spectrum>::Ray3f, Spectrum,
                       const typename Scene<Float, Spectrum>::EmitterPtr>
Scene<Float, Spectrum>::sample_emitter_ray(Float time, Float sample1,
                                           const Point2f &sample2,
                                           const Point2f &sample3,
                                           Mask active) const {
    MTS_MASKED_FUNCTION(ProfilerPhase::SampleEmitterRay, active);
    using EmitterPtr = ek::replace_scalar_t<Float, Emitter*>;

    if (unlikely(m_emitters.empty()))
        return std::make_tuple(Ray3f(), Spectrum(0.f), EmitterPtr(nullptr));

    // Randomly pick an emitter according to the precomputed emitter distribution
    auto [index, emitter_weight, sample_re] = sample_emitter_reuse(sample1, active);
    sample1 = sample_re;
    EmitterPtr emitter = ek::gather<EmitterPtr>(m_emitters.data(), index, active);

    // Wavelengths weight includes emitted radiance.
    // TODO: update if wavelength sampling is changed
#if 0
    auto [wavelengths, wav_weight] =
        emitter->sample_wavelengths(sample1, active);
    auto [ray, ray_weight] =
        emitter->sample_ray(time, wavelengths, sample2, sample3, active);
    // Account for the discrete probability of sampling this emitter.
    ray_weight *= wav_weight * emitter_weight;
#else
    auto [ray, ray_weight] =
        emitter->sample_ray(time, sample1, sample2, sample3, active);
#endif

    return std::make_tuple(ray, ray_weight, emitter);
}

MTS_VARIANT std::pair<typename Scene<Float, Spectrum>::UInt32, Float>
Scene<Float, Spectrum>::sample_emitter(Float index_sample, Mask active) const {
    MTS_MASKED_FUNCTION(ProfilerPhase::SampleEmitter, active);

    size_t emitters_size = m_emitters.size();
    // Fast paths if there is less than two emitters
    if (unlikely(emitters_size == 0))
        return { UInt32(-1), 0.f };
    if (emitters_size == 1)
        return { 0, 1 };

    // Randomly pick an emitter
#if 0
    auto [index, emitter_pdf] = m_emitter_distr.sample_pmf(index_sample);
    return { index, ek::rcp(emitter_pdf) };
#else
    UInt32 index = ek::min(UInt32(index_sample * (ScalarFloat) emitters_size),
                           (uint32_t) emitters_size - 1);
    // pdf = 1 / emitters_size  =>  sampling_weight = emitters_size
    return { index, emitters_size };
#endif
}

MTS_VARIANT std::tuple<typename Scene<Float, Spectrum>::UInt32, Float, Float>
Scene<Float, Spectrum>::sample_emitter_reuse(Float index_sample,
                                             Mask active) const {
    MTS_MASKED_FUNCTION(ProfilerPhase::SampleEmitter, active);

    size_t emitters_size = m_emitters.size();
    // Fast paths if there is less than two emitters
    if (unlikely(emitters_size == 0))
        return { UInt32(-1), 0.f, index_sample };
    if (emitters_size == 1)
        return { 0, 1, index_sample };

#if 0
    // Randomly pick an emitter
    auto [index, emitter_pdf, sample_re] = m_emitter_distr.sample_reuse_pmf(index_sample);
    return { index, ek::rcp(emitter_pdf), sample_re };
#else
    UInt32 index = ek::min(UInt32(index_sample * (ScalarFloat) emitters_size),
                           (uint32_t) emitters_size - 1);
    // Rescale sample to lie in [0,1) again
    index_sample = (index_sample - index / (ScalarFloat) emitters_size) * emitters_size;
    // pdf = 1 / emitters_size  =>  sampling_weight = emitters_size
    return { index, emitters_size, index_sample };
#endif
}

MTS_VARIANT std::pair<typename Scene<Float, Spectrum>::DirectionSample3f, Spectrum>
Scene<Float, Spectrum>::sample_emitter_direction(const Interaction3f &ref, const Point2f &sample_,
                                                 bool test_visibility, Mask active) const {
    MTS_MASKED_FUNCTION(ProfilerPhase::SampleEmitterDirection, active);
    using EmitterPtr = ek::replace_scalar_t<Float, Emitter*>;

    Point2f sample(sample_);
    DirectionSample3f ds;
    Spectrum spec;

    size_t emitters_size = m_emitters.size();
    if (likely(emitters_size > 0)) {
        if (emitters_size == 1) {
            // Fast path if there is only one emitter
            std::tie(ds, spec) = m_emitters[0]->sample_direction(ref, sample, active);
            ek::schedule(ds, spec);
        } else {
            // Randomly pick an emitter
            auto [index, emitter_weight, sample_re] =
                sample_emitter_reuse(sample.x(), active);
            sample.x() = sample_re;
            // Account for the discrete probability of sampling this emitter
            ds.pdf *= ek::rcp(emitter_weight);
            spec *= emitter_weight;

            // Sample a direction towards the emitter
            EmitterPtr emitter = ek::gather<EmitterPtr>(m_emitters_ptr.data(), index, active);
            std::tie(ds, spec) = emitter->sample_direction(ref, sample, active);
        }

        active &= ek::neq(ds.pdf, 0.f);

        // Perform a visibility test if requested
        if (test_visibility && ek::any_or<true>(active)) {
            Ray3f ray(ref.p, ds.d, math::RayEpsilon<Float> * (1.f + ek::hmax(ek::abs(ref.p))),
                      ds.dist * (1.f - math::ShadowEpsilon<Float>), ref.time, ref.wavelengths);
            spec[ray_test(ray, active)] = 0.f;
        }
    } else {
        ds = ek::zero<DirectionSample3f>();
        spec = 0.f;
    }

    return { ds, spec };
}

MTS_VARIANT Float
Scene<Float, Spectrum>::pdf_emitter(UInt32 /*index*/, Mask /*active*/) const {
    size_t emitters_size = m_emitters.size();
    if (emitters_size == 0)
        return 0.f;
    return ek::rcp(emitters_size);
}

MTS_VARIANT Float
Scene<Float, Spectrum>::pdf_emitter_direction(const Interaction3f &ref,
                                              const DirectionSample3f &ds,
                                              Mask active) const {
    MTS_MASK_ARGUMENT(active);
    using EmitterPtr = ek::replace_scalar_t<Float, const Emitter *>;

    size_t emitters_size = m_emitters.size();
    if (emitters_size == 1) {
        // Fast path if there is only one emitter
        Float res = m_emitters[0]->pdf_direction(ref, ds, active);
        ek::schedule(res);
        return res;
    } else {
        UInt32 emitter_index = 0;  // TODO: infer the sampled emitter index
        return pdf_emitter(emitter_index, active) *
               ek::reinterpret_array<EmitterPtr>(ds.object)->pdf_direction(ref, ds, active);
    }
}

MTS_VARIANT void Scene<Float, Spectrum>::traverse(TraversalCallback *callback) {
    for (auto& child : m_children) {
        std::string id = child->id();
        if (id.empty() || string::starts_with(id, "_unnamed_"))
            id = child->class_()->name();
        callback->put_object(id, child.get());
    }
}

MTS_VARIANT void Scene<Float, Spectrum>::parameters_changed(const std::vector<std::string> &keys) {
    if (m_environment)
        m_environment->set_scene(this); // TODO use parameters_changed({"scene"})

    bool update_accel = false;
    for (auto &s : m_shapes) {
        if (string::contains(keys, s->id()) || string::contains(keys, s->class_()->name())) {
            update_accel = true;
            break;
        }
    }

    if (update_accel) {
        if constexpr (ek::is_cuda_array_v<Float>)
            accel_parameters_changed_gpu();
        else {
            // TODO update Embree BVH or Mitsuba kdtree if necessary
        }
    }

    // Checks whether any of the shape's parameters require gradient
    m_shapes_grad_enabled = false;
    if constexpr (ek::is_diff_array_v<Float>) {
        for (auto& s : m_shapes) {
            m_shapes_grad_enabled |= s->parameters_grad_enabled();
            if (m_shapes_grad_enabled) break;
        }
    }
}

MTS_VARIANT std::string Scene<Float, Spectrum>::to_string() const {
    std::ostringstream oss;
    oss << "Scene[" << std::endl
        << "  children = [" << std::endl;
    for (size_t i = 0; i < m_children.size(); ++i) {
        oss << "    " << string::indent(m_children[i], 4);
        if (i + 1 < m_children.size())
            oss << ",";
        oss <<  std::endl;
    }
    oss << "  ]"<< std::endl
        << "]";
    return oss.str();
}

void librender_nop() { }

MTS_IMPLEMENT_CLASS_VARIANT(Scene, Object, "scene")
MTS_INSTANTIATE_CLASS(Scene)
NAMESPACE_END(mitsuba)
