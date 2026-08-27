/* Copyright (c) 2008-2026 the MRtrix3 contributors.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Covered Software is provided under this License on an "as is"
 * basis, without warranty of any kind, either expressed, implied, or
 * statutory, including, without limitation, warranties that the
 * Covered Software is free of defects, merchantable, fit for a
 * particular purpose or non-infringing.
 * See the Mozilla Public License v. 2.0 for more details.
 *
 * For more details, see http://www.mrtrix.org/.
 */

#ifndef __dwi_tractography_recognition_refine_h__
#define __dwi_tractography_recognition_refine_h__

#include <map>
#include <memory>
#include <string>

#include "mrtrix.h"
#include "types.h"
#include "image.h"
#include "interp/linear.h"
#include "transform.h"
#include "dwi/tractography/streamline.h"
#include "dwi/tractography/recognition/bundle_matcher.h"

namespace MR
{
  namespace DWI
  {
    namespace Tractography
    {
      namespace Recognition
      {

        // Deciding which of a bundle's candidates are really members of it.
        //
        // Auto-tracking generates streamlines inside a bundle's territory and then
        // keeps the ones whose shape matches the atlas bundle. A single shape
        // distance is not enough, because "close to this bundle" and "belonging to
        // this bundle" are different questions: two tracts that share a territory
        // are both close to it. The corticospinal tract and the medial lemniscus
        // are the standard case - adjacent and near-parallel through the brainstem,
        // similar in length, and with endpoints only centimetres apart - and no
        // threshold on distance-to-CST separates them, because much of ML is
        // genuinely closer to CST than the looser members of CST itself.
        //
        // What separates them is asking which bundle each streamline is closest
        // to, rather than only how close it is to the one asked for. That is
        // competitive assignment below, and it is the reason this module exists;
        // the other four stages are cheap gates that tighten what a distance
        // threshold leaves through.

        //! Which stage discarded a streamline.
        enum class RefineStage {
          Kept = 0,
          Length,        //!< outside the atlas bundle's own length range
          Endpoints,     //!< an end did not land at the matching end of the bundle
          Distance,      //!< too far from the bundle's shape
          Population,    //!< runs where the tract is rare across the population
          Competitor,    //!< a neighbouring bundle is a better match
          Territory,     //!< part of its course lies on a neighbour's own ground
          Outlier        //!< far from the rest of what was kept
        };

        const char* refine_stage_name (RefineStage);


        struct RefineOptions { NOMEMALIGN
          //! Reject a streamline that a neighbouring bundle claims better.
          bool competitive = false;
          //! Let a competitor penalise a streamline's score instead of rejecting it.
          /*! The hard rule is all-or-nothing: a neighbour that fits a streamline
           *  slightly better deletes it outright, and one that fits it very much
           *  better does exactly the same. That puts competition on a different
           *  footing from every other test here, which all feed one distance that
           *  strictness then thresholds.
           *
           *  Scoring folds it in instead. A streamline's score is its distance to
           *  the bundle plus how far inside the margin a competitor got:
           *
           *      penalty = max (0, margin * d_target - d_competitor)
           *      score   = d_target + competitive_weight * penalty
           *
           *  Uncontested streamlines score exactly as before, so nothing changes for
           *  them. A contested one moves up the ranking in proportion to how much
           *  better the neighbour fits, and strictness - which is a percentile of
           *  that same ranking - drops the worst of them first. Competition becomes
           *  the complement of strictness rather than a separate verdict. */
          bool competitive_scoring = true;
          //! How heavily the penalty weighs against the distance, per mm of it.
          /*! Calibrated against the hard rule on the projection category, at a 15 mm
           *  acceptance distance, with the whole category competing:
           *
           *      weight        0     1     2     3     5     8    hard
           *      CST_L self  400   400   400   400   400   400     397
           *      ML_L as CST 333     0     0     0     0     0       0
           *      thalamic    400   400   400   235     0     0       0
           *
           *  Below 5 a streamline bent through the thalamus - contested by CPT_P_L
           *  and others but still within 15 mm of CST - survives, which is the case
           *  competition exists for. At 5 it does not, and sensitivity is 400/400 on
           *  every bundle measured, which the hard rule never reached. Above 5
           *  nothing further changes, so it is not a knife edge. */
          float competitive_weight = 5.0f;

          //! How much closer a competitor must be before it takes a streamline.
          /*! At 1.0 the nearest bundle simply wins, which makes the outcome of a
           *  near-tie arbitrary - and near-ties are the common case exactly where
           *  this matters, along the stretch where two bundles run together. Below
           *  1.0 the streamline stays with the bundle that was asked for unless a
           *  competitor is clearly closer. */
          float competitive_margin = 0.9f;

          //! Keep the closest this fraction of candidates, in place of a mm threshold.
          /*! NaN leaves the millimetre threshold in charge. Otherwise the
           *  acceptance distance becomes this percentile of the candidates' own
           *  distance distribution, which is what makes a single "strictness"
           *  control behave the same way on every bundle - the millimetre scale
           *  that means "strict" for one bundle rejects everything in another. */
          float keep_fraction = NaN;

          //! Drop streamlines beyond median + k.MAD from what was kept.
          /*! NaN disables it. Measured on the kept set's own distances, so it needs
           *  no threshold from the user: it removes the tail rather than a fixed
           *  distance. Lower k is stricter; 3.0/2.0/1.5 are offered as
           *  low/medium/high in the GUI. */
          float outlier_k = NaN;

          //! Bound length by the atlas bundle's own p2-p98 rather than its extremes.
          /*! Percentiles because an atlas bundle carries a few truncated and a few
           *  run-away streamlines, and the extremes are exactly those.
           *
           *  The bounds around them are deliberately lopsided, and the asymmetry is
           *  the point. A tracked streamline that covers only part of the bundle's
           *  course is still a member of it - the directed closest-point metric is
           *  built to accept exactly that - so the lower bound has to stay generous.
           *  A first version used p2-p98 with a symmetric 10% margin and it rejected
           *  whole bundles: measured on DTT_LR, trimming 10% off each end of its own
           *  atlas streamlines left 16% surviving, and trimming 20% left none at all.
           *  The upper bound can afford to be tight, because nothing legitimate is
           *  much longer than the bundle. */
          bool length_window = true;
          float length_low_ratio = 0.5f;    //!< of p2: how short a member may be
          float length_high_ratio = 1.25f;  //!< of p98: how long

          //! Fit the atlas bundle onto the candidates before measuring shape.
          /*! Where the atlas sits is only as good as the registration that put it
           *  there, and the residual offset varies bundle to bundle. The alternative
           *  already here is to *loosen the threshold* when that happens (the p10
           *  rule below), which compensates for a registration error rather than
           *  correcting it - and a looser threshold lets a neighbouring bundle in.
           *
           *  This instead fits the bundle to the candidates with a rigid transform
           *  plus a uniform scale, then matches strictly. RecoBundles does the same
           *  thing between its reduction and pruning passes.
           *
           *  MEASURED AND LEFT OFF. On held-out atlas halves at 8 mm Hausdorff,
           *  against CST_L with ML_L as the rival, all at 100% sensitivity:
           *
           *      loosening (the incumbent)     aligned 57% leakage   offset 20.75%
           *      no loosening                  aligned  5.5%         offset  0%
           *      local fit                     aligned 14.5%         offset 29%
           *
           *  It loses on both, and worse where it was supposed to help: the fit is
           *  solved against the surviving candidates, which still contain the rival
           *  bundle, so fitting CST's atlas onto them drags it towards ML and ML then
           *  matches better. A tighter bound does not fix that - the drag measured
           *  4.5 mm, well inside any bound that would still allow a useful
           *  correction. Fitting against only the *confidently* matched candidates
           *  might, and would be the thing to try next.
           *
           *  Kept because the measurement is the valuable part: it is what showed the
           *  incumbent's real cost, and adaptive_loosening below is the setting that
           *  came out of it. Reachable from tckrefine -fit, off everywhere else. */
          bool local_registration = false;
          //! Let a poorly placed atlas loosen the acceptance distance.
          /*! The p10 rule in refine.cpp: when the closest candidates all sit well
           *  away from the atlas, read that as a registration offset and accept a
           *  looser distance. It buys sensitivity for a badly placed bundle and pays
           *  in specificity, because a looser distance is exactly what lets a
           *  neighbouring bundle through. */
          bool adaptive_loosening = true;
          //! Refuse a fit beyond these bounds, in mm / degrees / ratio.
          /*! A fit is only meant to absorb residual registration error. Left
           *  unbounded it can drag the bundle onto whatever unrelated streamlines
           *  happen to dominate the candidate set, which would make the match
           *  agree with anything. */
          float max_fit_translation = 15.0f;
          float max_fit_rotation = 20.0f;
          float max_fit_scale = 1.25f;

          //! Score each streamline by its worst point against the bundle core.
          /*! The whole-streamline outlier test above reduces a streamline to one
           *  number, so a streamline that follows the bundle and then peels off at one
           *  end averages out as acceptable. This measures the deviation from the
           *  bundle's core at each point along it - the AFQ/DIPY formulation - and
           *  scores the streamline by its worst point. Uses outlier_k as its
           *  threshold, in standard deviations. */
          bool per_node_outliers = false;

          //! Require each end near the *corresponding* end of the atlas bundle.
          /*! BundleMatcher's endpoint pre-filter asks only that both ends be near
           *  some terminus of the bundle, so a streamline that leaves and returns to
           *  the same end passes it. This pairs the ends up instead.
           *
           *  It rejects only a streamline that leaves and returns to the same end,
           *  never one that merely stops short of a terminus - see EndpointGate. */
          bool endpoint_gate = false;
          float endpoint_radius = 8.0f;

          //! Reject a streamline that spends part of its course on a neighbour's ground.
          /*! Competition decides per streamline; overlap is per point. A candidate
           *  that runs with the target for most of its length and crosses into a
           *  neighbour for the rest wins the per-streamline comparison and is kept,
           *  and it is what puts one tract's fibres inside another's exclusive
           *  territory - measured on CST_L against DRTT_L, a candidate whose top
           *  half follows DRTT is kept 400 of 400 with DRTT competing.
           *
           *  Needs competitors, and a metric with a vertex grid (not MDF). */
          //! Share of its own streamlines a competitor may take before it is dropped.
          /*! Guards against a category that holds a bundle and its parts; see
           *  CompetitorSet::drop_self_claimants(). At or above 1.0 nothing is
           *  dropped, which is how to measure what this is worth. */
          float competitor_self_claim_limit = 0.5f;

          //! Probability below which a voxel counts as outside the tract.
          /*! The population maps run 0 to 1, so 0.05 means "fewer than one subject
           *  in twenty has this tract here". */
          float population_min_probability = 0.05f;
          //! Share of the streamline's course allowed outside, before it is rejected.
          /*! At or above 1.0 the test is off, which is the default and also what
           *  happens when no map is supplied.
           *
           *  MEASURED AND LEFT OFF, and the measurement is worth keeping because the
           *  reasoning behind it was wrong in an instructive way. The maps looked like
           *  an escape from the atlas's tracking method: they make no demand on a
           *  candidate's shape, only on where it goes, and they aggregate 1065
           *  subjects. Against held-out atlas halves they are spectacular - CST_L
           *  keeps 381 of its own 400 while ML_L, DRTT_L, DTT_LR, AF_L and a
           *  streamline bent through the thalamus are every one of them 0 of 400,
           *  with no competitors at all and in 0.23 s against 15 s.
           *
           *  But held-out halves are deterministic streamlines, so that harness cannot
           *  see the one thing this was for. Applying the wander that separates
           *  probabilistic tracking from deterministic - smooth, low-frequency, tract-
           *  preserving - the map degrades *faster* than the shape distance it was
           *  meant to replace:
           *
           *      RMS wander      0mm   2mm   4mm   6mm   8mm
           *      shape+category  397   359   261   191   143
           *      population map  381   260    99    21     4
           *
           *  It inherits the tracking method after all, in extent rather than in
           *  shape: 1065 deterministic tractograms all follow dominant peaks, so the
           *  high-probability core is narrow, and a streamline wandering 4 mm leaves
           *  it. Cross-subject anatomy widens that core; within-subject dispersion is
           *  a different thing and the map does not represent it.
           *
           *  Nor is it a calibration problem. Sweeping the pair, every setting loose
           *  enough to keep dispersed streamlines also keeps the thalamic bulge:
           *
           *      outside <=      0.10          0.20          0.30
           *      disp 4mm / bulge  99 /   0     188 / 400     285 / 400
           *
           *  The two distributions overlap - a legitimately dispersed streamline and
           *  one that took a wrong turn spend a similar share of their course outside
           *  the deterministic core - so no threshold separates them. Reachable
           *  through tckrefine -population. */
          float population_max_outside = 1.0f;
          //! Share of each end left out of the measurement.
          /*! A tract's ends legitimately leave its core - they fan into cortex, where
           *  no two subjects agree and the population probability is near zero. Left
           *  in, the ends alone put a genuine streamline over any useful threshold. */
          float population_trim = 0.10f;

          bool exclusive_territory = false;
          //! How much nearer the neighbour a point must be to count as its ground, mm.
          /*! Ground the two bundles genuinely share counts for neither, which is what
           *  keeps this from firing along the course they legitimately run together. */
          float exclusive_margin = 4.0f;
          //! Share of the streamline's samples allowed on a neighbour's ground.
          float exclusive_fraction = 0.05f;
          //! Samples taken along a candidate for the territory test.
          static constexpr size_t exclusive_samples = 60;
        };


        //! A bundle that might claim a streamline away from the one being refined.
        struct Competitor { NOMEMALIGN
          std::string name;
          vector<Streamline<float>> bundle;
        };


        struct RefineReport { NOMEMALIGN
          size_t input = 0, kept = 0;
          //! The local fit that was applied, for reporting; identity if none was.
          float fit_translation = 0.0f, fit_rotation = 0.0f, fit_scale = 1.0f;
          bool fit_applied = false;
          //! Streamlines removed, per stage.
          std::map<RefineStage, size_t> removed;
          //! Acceptance distance actually applied, in mm.
          float applied_distance = 0.0f;
          //! Outlier cut-off actually applied, in mm; 0 if pruning was off.
          float outlier_cutoff = 0.0f;
          //! Length window applied, in mm.
          float min_length = 0.0f, max_length = 0.0f;
          //! How many streamlines each competitor took, for reporting which
          //  neighbour a bundle is being confused with.
          std::map<std::string, size_t> claimed_by;

          size_t removed_by (RefineStage stage) const {
            const auto it = removed.find (stage);
            return it == removed.end() ? 0 : it->second;
          }
          std::string summary () const;
        };


        //! The two terminal clouds of a bundle, ends paired up consistently.
        /*! Streamline direction is arbitrary in a track file, so each streamline is
         *  first oriented against the longest one; without that each "cloud" would
         *  hold a mixture of both ends. */
        void endpoint_clouds (const vector<Streamline<float>>& tracks,
                              vector<Eigen::Vector3f>& cloud_a,
                              vector<Eigen::Vector3f>& cloud_b);


        //! The p\a low to p\a high range of a bundle's streamline lengths, in mm.
        /*! Percentiles rather than the extremes: an atlas bundle usually carries a
         *  few truncated or run-away streamlines, and the extremes are exactly those. */
        void bundle_length_window (const vector<Streamline<float>>& bundle,
                                   float& min_length, float& max_length,
                                   float low = 0.02f, float high = 0.98f);


        //! Requires a streamline's ends to land where the bundle terminates.
        class EndpointGate { MEMALIGN(EndpointGate)
          public:
            /*! \a paired requires one end at each terminus of the bundle, taken
             *  either way round. Unpaired only asks that both ends be near some
             *  terminus, which is what BundleMatcher's own pre-filter does and what
             *  lets a streamline that leaves and returns to the same end through. */
            EndpointGate (const vector<Streamline<float>>& bundle, float radius, bool paired);
            bool accepts (const Streamline<float>&) const;
            bool usable () const { return usable_; }
          private:
            const float radius_;
            const bool paired_;
            bool usable_;
            VoxelHashGrid cloud_a_, cloud_b_;
        };


        //! The competing bundles, each compressed to a matcher of its own.
        class CompetitorSet { MEMALIGN(CompetitorSet)
          public:
            //! \a params must be the ones the target bundle is matched with, or the
            //  two distances are not comparable.
            CompetitorSet (const vector<Competitor>&, const BundleMatcher::Params& params);

            //! Distance to the closest competing bundle, and which it was.
            /*! Infinity, with \a name left empty, when no competitor will have it.
             *  Consults every competitor, so it is the form to use for reporting. */
            float nearest (const Streamline<float>& tck, std::string& name) const;

            //! How far inside \a limit the closest competitor gets, and which it was.
            /*! Zero when none of them is inside it, which is the common case and the
             *  cheap one: each competitor is first asked the yes/no question, and
             *  only one that gets inside the running best is measured exactly. */
            float penalty (const Streamline<float>& tck, float limit, std::string& name) const;

            //! True if some competitor is closer than \a limit, naming the first found.
            /*! Stops at the first one that qualifies, which is what makes a wide
             *  competitor set affordable: the neighbour that takes a streamline is
             *  usually the one that overlaps the target most, and competitors arrive
             *  in that order. Costs a full pass only for streamlines nobody claims. */
            bool claims (const Streamline<float>& tck, float limit, std::string& name) const;

            //! Share of a candidate's course that lies in a competitor's own ground.
            /*! Competition by \a claims() is a decision about a whole streamline, but
             *  overlap is a property of points: a candidate that follows the target
             *  for most of its length and crosses into a neighbour for the rest is
             *  closest to the target on average, so it is kept - and it paints the
             *  neighbour's territory, in voxels where the two atlas bundles do not
             *  meet. This asks the same question the other way round, sample by
             *  sample: how much of this streamline sits at least \a margin nearer some
             *  competitor than it does the target.
             *
             *  Returns the largest share over the competitors, naming that one.
             *  Zero when there is no vertex grid to ask (the MDF metric builds none).
             *
             *  \a samples is deliberately far denser than the shape metric's 20: a
             *  crossing into a neighbour's ground is often a short stretch, and at 20
             *  samples one of them is already 5% of the streamline, so the threshold
             *  would be quantised to the point of meaninglessness. */
            float intrusion (const Streamline<float>& tck,
                             const BundleMatcher& target,
                             float margin,
                             size_t samples,
                             std::string& name) const;

            //! Remove competitors that claim the bundle's own streamlines.
            /*! A category holds bundles at more than one granularity: SLF_L is the
             *  union of SLF1_L, SLF2_L and SLF3_L, CC of its segments, C_L of its
             *  parts. A part is not a rival of its parent - it is the same anatomy
             *  named twice - and letting it compete deletes the parent outright.
             *  Measured: with SLF1_L competing, SLF_L keeps 0 of its own 400.
             *
             *  Detected rather than named, because a naming convention belongs to one
             *  atlas: half the bundle is held out, matched against the other half, and
             *  a competitor taking more than \a max_share of it is dropped. The
             *  separation is not close - on this atlas a part takes all 400 and the
             *  hardest genuine rivals take three (DRTT_L against NDTT_L, CST_L against
             *  CPT_P_L) - so the threshold is not a tuned parameter.
             *
             *  Returns how many were dropped, appending their names to \a dropped. */
            size_t drop_self_claimants (const vector<Streamline<float>>& atlas,
                                        const BundleMatcher::Params& params,
                                        float competitive_margin,
                                        float max_share,
                                        vector<std::string>& dropped);

            size_t size () const { return matchers_.size(); }
            bool empty () const { return matchers_.empty(); }

          private:
            vector<std::string> names_;
            vector<std::unique_ptr<BundleMatcher>> matchers_;
        };


        //! Fit \a bundle onto \a candidates with a rigid transform plus uniform scale.
        /*! Correspondence-free: a few rounds of "match each bundle point to its
         *  nearest candidate point, then solve the best transform for those pairs".
         *  Returns false, leaving \a transform untouched, when the fit exceeds the
         *  bounds in \a options or there is too little to fit from. */
        bool fit_bundle_to_candidates (const vector<Streamline<float>>& bundle,
                                       const vector<Streamline<float>>& candidates,
                                       const RefineOptions& options,
                                       Eigen::Matrix4f& transform,
                                       float& translation, float& rotation, float& scale);

        //! Apply a 4x4 transform to every vertex of every streamline.
        vector<Streamline<float>> transform_streamlines (const vector<Streamline<float>>&,
                                                         const Eigen::Matrix4f&);

        //! Per-point deviation from the bundle core, in standard deviations.
        /*! One value per streamline: the largest deviation over its points. Streamlines
         *  are oriented consistently first, since a reversed streamline would otherwise
         *  be compared head-to-tail against the core. */
        vector<float> per_node_core_distance (const vector<Streamline<float>>& tracks,
                                              size_t num_points = 20);

        //! A tract's population probability, as a voxel map.
        /*! The reference the shape metric uses is a set of streamlines, and comparing
         *  against streamlines asks whether a candidate *looks like* one - which is a
         *  question about the algorithm that drew the atlas, not about anatomy. The
         *  HCP1065 atlas was tracked deterministically; MRView++ tracks with iFOD2,
         *  and probabilistic tracking spreads wider, which is most of what the
         *  acceptance distance has been absorbing.
         *
         *  A population map asks a different question: of 1065 subjects, how many have
         *  this tract in this voxel. It makes no demand on shape, so a streamline is
         *  judged by where it goes rather than by how it was drawn.
         *
         *  Not a full escape - the maps were themselves aggregated from one pipeline's
         *  output - but aggregating 1065 subjects turns an algorithm's output into a
         *  population frequency, and that is the part that transfers. */
        class PopulationMap { MEMALIGN(PopulationMap)
          public:
            /*! \a to_map takes a point in the candidates' space into the map's. For a
             *  subject tracked in scanner space that is the inverse of the atlas fit;
             *  identity when candidates and map already share a space. Throws if the
             *  image cannot be opened. */
            PopulationMap (const std::string& path,
                           const transform_type& to_map = transform_type::Identity());

            //! Probability at a point given in the candidates' space; 0 outside.
            float at (const Eigen::Vector3f& p);

            const std::string& path () const { return path_; }

          private:
            std::string path_;
            transform_type to_map_;
            Image<float> image_;
            Interp::Linear<Image<float>> interp_;
        };


        //! Share of a streamline's course spent where the tract is rare.
        /*! \a trim of the streamline is dropped from each end before measuring; see
         *  RefineOptions::population_trim. Returns 0 for a streamline too short to
         *  leave anything after trimming, since nothing has been shown against it. */
        float population_outside (const Streamline<float>& tck,
                                  PopulationMap& map,
                                  float min_probability,
                                  float trim);


        //! Keep the candidates that belong to \a atlas, and say why the rest went.
        /*! The stages run cheapest-first, so the expensive ones only ever see what
         *  survived the cheap ones:
         *
         *    length -> endpoints -> shape distance -> competitors -> outliers
         *
         *  \a competitors may be empty, in which case that stage is skipped
         *  whatever RefineOptions::competitive says. \a rejected receives everything
         *  discarded, in candidate order, and \a stages (when given) receives one
         *  RefineStage per input candidate so a caller can show what removed what. */
        void refine_bundle (const vector<Streamline<float>>& atlas,
                            const BundleMatcher::Params& match,
                            const RefineOptions& options,
                            const vector<Competitor>& competitors,
                            const vector<Streamline<float>>& candidates,
                            vector<Streamline<float>>& kept,
                            vector<Streamline<float>>& rejected,
                            RefineReport& report,
                            vector<RefineStage>* stages = nullptr,
                            PopulationMap* population = nullptr);

      }
    }
  }
}

#endif
