/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DE265_DECCTX_H
#define DE265_DECCTX_H

#include "libde265/vps.h"
#include "libde265/sps.h"
#include "libde265/pps.h"
#include "libde265/nal.h"
#include "libde265/slice.h"
#include "libde265/image.h"
#include "libde265/motion.h"
#include "libde265/de265.h"
#include "libde265/dpb.h"
#include "libde265/sei.h"
#include "libde265/threads.h"
#include "libde265/acceleration.h"
#include "libde265/nal-parser.h"
#include "libde265/image-unit.h"
#include "libde265/frame-dropper.h"

#include <memory>

#define DE265_MAX_VPS_SETS 16   // this is the maximum as defined in the standard
#define DE265_MAX_SPS_SETS 16   // this is the maximum as defined in the standard
#define DE265_MAX_PPS_SETS 64   // this is the maximum as defined in the standard

#define MAX_WARNINGS 20


class slice_segment_header;
class image_unit;
class slice_unit;
class decoder_context;

//typedef std::shared_ptr<image_unit> image_unit_ptr;


class thread_context
{
public:
  thread_context();

  int CtbAddrInRS;
  int CtbAddrInTS;

  int CtbX, CtbY;


  // motion vectors

  PBMotionCoding motion;


  // prediction

  // enum IntraPredMode IntraPredModeC[4]; // chroma intra-prediction mode for current CB
  int ResScaleVal;


  // residual data

  uint8_t cu_transquant_bypass_flag;
  uint8_t transform_skip_flag[3];
  uint8_t explicit_rdpcm_flag;
  uint8_t explicit_rdpcm_dir;

  ALIGNED_16(int16_t) _coeffBuf[(32*32)+8]; // alignment required for SSE code !
  int16_t *coeffBuf;

  int16_t coeffList[3][32*32];
  int16_t coeffPos[3][32*32];
  int16_t nCoeff[3];

  int32_t residual_luma[32*32]; // only used when cross-comp-prediction is enabled


  // quantization

  int IsCuQpDeltaCoded;
  int CuQpDelta;
  int IsCuChromaQpOffsetCoded;
  int CuQpOffsetCb, CuQpOffsetCr;

  int currentQPY;
  int currentQG_x, currentQG_y;
  int lastQPYinPreviousQG;

  int qPYPrime, qPCbPrime, qPCrPrime;

  CABAC_decoder cabac_decoder;

  context_model_table ctx_model;
  uint8_t StatCoeff[4];

  decoder_context* decctx;
  image_ptr img;
  slice_segment_header* shdr;

  image_unit* imgunit;
  slice_unit* sliceunit;
  thread_task* task; // executing thread_task or NULL if not multi-threaded

private:
  thread_context(const thread_context&); // not allowed
  const thread_context& operator=(const thread_context&); // not allowed
};



class error_queue
{
 public:
  error_queue();

  void add_warning(de265_error warning, bool once);
  de265_error get_warning();

 private:
  de265_error warnings[MAX_WARNINGS];
  int nWarnings;
  de265_error warnings_shown[MAX_WARNINGS]; // warnings that have already occurred
  int nWarningsShown;
};



class image_history
{
 public:
  virtual ~image_history() { }

  //virtual /* */ de265_image* get_image(int dpb_index)       { return dpb.get_image(dpb_index); }
  virtual std::shared_ptr<const image> get_image(int frame_id) const = 0;
  virtual bool has_image(int frame_id) const = 0;
};



class base_context : public error_queue,
                     public image_history
{
 public:
  base_context();
  virtual ~base_context() { }

  // --- accelerated DSP functions ---

  void set_acceleration_functions(enum de265_acceleration);

  struct acceleration_functions acceleration; // CPU optimized functions
};



class image_unit_sink;


class frontend_syntax_decoder : private on_NAL_inserted_listener
{
 public:
  frontend_syntax_decoder(decoder_context* ctx);

  void reset();


  // --- frontend for pushing data into the decoder ---

  bool is_input_buffer_full() const;

  // Pushing data into the NAL-parser will automatically forward the complete NALs
  // the the frontend_syntax_decoder, where they are subsequently combines into image_units.
  NAL_Parser& get_NAL_parser() { return nal_parser; }


  // Complete image_units are forwarded to this sink.
  void set_image_unit_sink(image_unit_sink* sink) { m_image_unit_sink = sink; }



  // --- query the current state ---

  bool has_sps(int id) const { return (bool)sps[id]; }
  bool has_pps(int id) const { return (bool)pps[id]; }

  /* */ seq_parameter_set* get_sps(int id)       { return sps[id].get(); }
  const seq_parameter_set* get_sps(int id) const { return sps[id].get(); }
  /* */ pic_parameter_set* get_pps(int id)       { return pps[id].get(); }
  const pic_parameter_set* get_pps(int id) const { return pps[id].get(); }
  std::shared_ptr<const seq_parameter_set> get_sps_ptr(int id) const { return sps[id]; }
  std::shared_ptr<const pic_parameter_set> get_pps_ptr(int id) const { return pps[id]; }

  std::shared_ptr<seq_parameter_set>  get_current_sps() { return current_sps; }

  // get highest temporal sub-layer ID
  int  get_highest_TID() const;


  // The previously decoded slice header (so that we can copy it in dependent slices)
  bool has_previous_slice_header() const { return previous_slice_header != NULL; }
  const slice_segment_header& get_previous_slice_header() { return *previous_slice_header; }


  // --- modify state by slice header ---

  void set_PocLsbLt(int picIdx, int lsb) { PocLsbLt[picIdx] = lsb; }
  void set_UsedByCurrPicLt(int picIdx, int used) { UsedByCurrPicLt[picIdx] = used; }
  void set_DeltaPocMsbCycleLt(int picIdx, int delta) { DeltaPocMsbCycleLt[picIdx] = delta; }

  bool is_UsedByCurrPicLt(int picIdx) { return UsedByCurrPicLt[picIdx]; }
  int  get_DeltaPocMsbCycleLt(int picIdx) { return DeltaPocMsbCycleLt[picIdx]; }


  // --- debugging ---

  int  param_sps_headers_fd;
  int  param_vps_headers_fd;
  int  param_pps_headers_fd;
  int  param_slice_headers_fd;



  // --- (TODO) make this private and reorganize ---

  void debug_imageunit_state();

 private:
  de265_error decode_NAL(NAL_unit* nal);

  de265_error read_vps_NAL(bitreader&);
  de265_error read_sps_NAL(bitreader&);
  de265_error read_pps_NAL(bitreader&);
  de265_error read_sei_NAL(bitreader& reader, bool suffix);
  de265_error read_eos_NAL(bitreader& reader);
  de265_error read_slice_NAL(bitreader&, NAL_unit* nal, nal_header& nal_hdr);

  bool process_slice_segment_header(slice_segment_header*,
                                    de265_error*, de265_PTS pts,
                                    nal_header* nal_hdr, void* user_data);


  // ---

  decoder_context* m_decctx;
  image_unit_sink* m_image_unit_sink;


  // --- input stream data ---

  int  current_image_poc_lsb;
  bool first_decoded_picture;
  bool NoRaslOutputFlag;
  bool HandleCraAsBlaFlag;
  bool FirstAfterEndOfSequenceNAL;

  bool flush_reorder_buffer_at_this_frame;

  int PicOrderCntMsb;
  int prevPicOrderCntLsb;  // at precTid0Pic
  int prevPicOrderCntMsb;  // at precTid0Pic

  const slice_segment_header* previous_slice_header; /* Remember the last slice for a successive
                                                        dependent slice. */


  // --- motion compensation ---

  int PocLsbLt[MAX_NUM_REF_PICS];
  int UsedByCurrPicLt[MAX_NUM_REF_PICS];
  int DeltaPocMsbCycleLt[MAX_NUM_REF_PICS];

  int CurrDeltaPocMsbPresentFlag[MAX_NUM_REF_PICS];
  int FollDeltaPocMsbPresentFlag[MAX_NUM_REF_PICS];

  // The number of entries in the lists below.
  int NumPocStCurrBefore;
  int NumPocStCurrAfter;
  int NumPocStFoll;
  int NumPocLtCurr;
  int NumPocLtFoll;

  // These lists contain absolute POC values.
  int PocStCurrBefore[MAX_NUM_REF_PICS]; // used for reference in current picture, smaller POC
  int PocStCurrAfter[MAX_NUM_REF_PICS];  // used for reference in current picture, larger POC
  int PocStFoll[MAX_NUM_REF_PICS]; // not used for reference in current picture, but in future picture
  int PocLtCurr[MAX_NUM_REF_PICS]; // used in current picture
  int PocLtFoll[MAX_NUM_REF_PICS]; // used in some future picture

  // These lists contain indices into the DPB.
  int RefPicSetStCurrBefore[MAX_NUM_REF_PICS];
  int RefPicSetStCurrAfter[MAX_NUM_REF_PICS];
  int RefPicSetStFoll[MAX_NUM_REF_PICS];
  int RefPicSetLtCurr[MAX_NUM_REF_PICS];
  int RefPicSetLtFoll[MAX_NUM_REF_PICS];

  void process_picture_order_count(slice_segment_header* hdr);
  int  generate_unavailable_reference_picture(const seq_parameter_set* sps,
                                              int POC, bool longTerm);
  void process_reference_picture_set(slice_segment_header* hdr);
  bool construct_reference_picture_lists(slice_segment_header* hdr);



  // --- parameters derived from parameter sets ---


  // --- current NAL ---

  NAL_Parser nal_parser;

  uint8_t nal_unit_type;

  //char IdrPicFlag; // unused ?
  char RapPicFlag;


  // --- building the next image_unit ---

  image_unit_ptr m_curr_image_unit;
  image_ptr m_curr_img;


  // --- internal data ---

  std::shared_ptr<video_parameter_set>  vps[ DE265_MAX_VPS_SETS ];
  std::shared_ptr<seq_parameter_set>    sps[ DE265_MAX_SPS_SETS ];
  std::shared_ptr<pic_parameter_set>    pps[ DE265_MAX_PPS_SETS ];

  std::shared_ptr<video_parameter_set>  current_vps;
  std::shared_ptr<seq_parameter_set>    current_sps;
  std::shared_ptr<pic_parameter_set>    current_pps;

  // on_NAL_inserted_listener

  virtual de265_error on_NAL_inserted();
  virtual void on_end_of_stream();
  virtual void on_end_of_frame();
};




class decoder_context : public base_context,
                        public image_unit_sink
{
 public:
  decoder_context();
  ~decoder_context();

  de265_error start_thread_pool(int nThreads);
  void        stop_thread_pool();

  void reset();


  // -------------------------------------------------- frontend_syntax_decoder

  NAL_Parser& get_NAL_parser() { return m_frontend_syntax_decoder.get_NAL_parser(); }
  frontend_syntax_decoder& get_frontend_syntax_decoder() { return m_frontend_syntax_decoder; }
  const frontend_syntax_decoder& get_frontend_syntax_decoder() const { return m_frontend_syntax_decoder; }

 private:
  frontend_syntax_decoder m_frontend_syntax_decoder;




  // -------------------------------------------------- image_unit classifier

  frame_dropper_nop        m_frame_dropper_nop;
  frame_dropper_IRAP_only  m_frame_dropper_IRAP_only;
  frame_dropper_ratio      m_frame_dropper_ratio;


  // -------------------------------------------------- decoding main loop

  // --- image_unit input queue ---

  virtual void send_image_unit(image_unit_ptr imgunit);
  virtual void send_end_of_stream();


 public:
  // --- decoding loop ---

  de265_error decode_image_unit(bool* did_work);

  // --- frame-parallel decoding ---

  void start_decoding_thread();
  void stop_decoding_thread();

 private:
  de265_error decode_slice_unit_sequential(image_unit* imgunit, slice_unit* sliceunit);
  de265_error decode_slice_unit_parallel(image_unit* imgunit, slice_unit* sliceunit);
  de265_error decode_slice_unit_WPP(image_unit* imgunit, slice_unit* sliceunit);
  de265_error decode_slice_unit_tiles(image_unit* imgunit, slice_unit* sliceunit);


  de265_error push_picture_to_output_queue(image_ptr);


 public:
  // --- parameters ---

  bool param_sei_check_hash;
  bool param_conceal_stream_errors;
  bool param_suppress_faulty_pictures;

  bool param_disable_deblocking;
  bool param_disable_sao;
  //bool param_disable_mc_residual_idct;  // not implemented yet
  //bool param_disable_intra_residual_idct;  // not implemented yet

  void set_frame_dropping_ratio(float ratio);


  void set_image_allocation_functions(de265_image_allocation* allocfunc);

  de265_image_allocation param_image_allocation_functions;



  int get_num_worker_threads() const { return num_worker_threads; }

  std::shared_ptr</* */ image> get_image(int dpb_index)       { return dpb.get_image(dpb_index); }
  std::shared_ptr<const image> get_image(int dpb_index) const { return dpb.get_image(dpb_index); }

  bool has_image(int dpb_index) const { return dpb_index>=0 && dpb_index<dpb.size(); }

  image_ptr get_next_picture_in_output_queue() { return m_output_queue.get_next_picture_in_output_queue(); }
  int    num_pictures_in_output_queue() const { return m_output_queue.num_pictures_in_output_queue(); }
  void   pop_next_picture_in_output_queue() { m_output_queue.pop_next_picture_in_output_queue(); }


  void debug_imageunit_state();

 public:
  thread_pool thread_pool_;

 private:
  int num_worker_threads;


  // --- main loop ---

  class thread_main_loop : public de265_thread_class {
  public:
    thread_main_loop(decoder_context* dctx) : m_decctx(dctx) { }
    void run() { while (!should_stop()) { m_decctx->run_main_loop(); } }
  private:
    decoder_context* m_decctx;
  };

  thread_main_loop m_main_loop_thread;
  de265_mutex m_main_loop_mutex;
  de265_cond  m_main_loop_full_cond;
  de265_cond  m_input_empty_cond;

  void run_main_loop();

  std::vector<image_unit_ptr> m_image_units_in_progress;
  static const int m_max_images_processed_in_parallel = 4;

  void decode_image_frame_parallel(image_unit_ptr imgunit);


 public:
  // --- frame dropping ---

  void set_limit_TID(int tid);
  int  get_current_TID() const { return current_HighestTid; }
  int  change_framerate(int more_vs_less); // 1: more, -1: less
  void set_framerate_ratio(int percent);

 private:
  // input parameters
  int limit_HighestTid;    // never switch to a layer above this one
  int framerate_ratio;

  // current control parameters
  int goal_HighestTid;     // this is the layer we want to decode at
  int layer_framerate_ratio; // ratio of frames to keep in the current layer

  int current_HighestTid;  // the layer which we are currently decoding

  struct {
    int8_t tid;
    int8_t ratio;
  } framedrop_tab[100+1];
  int framedrop_tid_index[6+1];

  void compute_framedrop_table();

 public:
  void calc_tid_and_framerate_ratio();

 private:
  // --- decoded picture buffer ---

 public:
  decoded_picture_buffer dpb;
  picture_output_queue   m_output_queue;


  // --- image unit queue ---

 public:
  std::vector<image_unit_ptr> image_units;

  bool m_end_of_stream;

 private:
  void init_thread_context(thread_context* tctx);
  void add_task_decode_CTB_row(thread_context* tctx, bool firstSliceSubstream, int ctbRow);
  void add_task_decode_slice_segment(thread_context* tctx, bool firstSliceSubstream,
                                     int ctbX,int ctbY);

  void mark_whole_slice_as_processed(image_unit* imgunit,
                                     slice_unit* sliceunit,
                                     int progress);

  void remove_images_from_dpb(const std::vector<int>& removeImageList);
  void run_postprocessing_filters_sequential(image_ptr img);
  void run_postprocessing_filters_parallel(image_unit* img);
};


#endif
