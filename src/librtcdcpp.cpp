/**
 * C Wrapper around the C++ classes.
 */

#include <unistd.h>
#include <strings.h>
#include <stdio.h>

#include <rtcdcpp/PeerConnection.hpp>
#include <rtcdcpp/callbacks.pb.h>
#include <rtcdcpp/librtcdcpp.h>
#include <rtcdcpp/DataChannel.hpp>
#include <glib.h>
#include <stdbool.h>
#include <memory>
#include <functional>
#include <iostream>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <list>
#include <unordered_map>
#include <zmq.h>
#include <thread>
#include <errno.h>
#define DESTROY_PC 0
#define PARSE_SDP 1
#define GENERATE_OFFER 2
#define GENERATE_ANSWER 3
#define SET_REMOTE_ICE_CAND 4
#define SET_REMOTE_ICE_CANDS 5
#define CREATE_DC 6
#define GET_DC_SID 7
#define GET_DC_TYPE 8
#define GET_DC_LABEL 9
#define GET_DC_PROTO 10
#define SEND_STRING 11
#define SEND_BINARY 12
#define CLOSE_DC 13

extern "C" {
  IceCandidate_C* newIceCandidate(const char* candidate, const char* sdpMid, int sdpMLineIndex) {
    //IceCandidate_C ice_cand;
    IceCandidate_C* ice_cand = (IceCandidate_C *) malloc(sizeof(IceCandidate_C));
    ice_cand->candidate = candidate; //&
    ice_cand->sdpMid = sdpMid;
    ice_cand->sdpMLineIndex = sdpMLineIndex;
    return ice_cand;
  }

  void sendSignal(void* zmqsock) {
    zmq_send (zmqsock, "", 0, 0);
  }

  void signalSink(void *zmqsock) {
    void* nothing;
    zmq_recv (zmqsock, nothing, 0, 0);
  }

  std::list<int> process_status;

  void fillInCandidate(librtcdcpp::Callback* callback, std::string candidate, std::string sdpMid, int sdpMLineIndex) {
    librtcdcpp::Callback::onCandidate* on_candidate = new librtcdcpp::Callback::onCandidate;
    on_candidate->set_candidate(candidate.c_str());
    on_candidate->set_sdpmid(sdpMid.c_str());
    on_candidate->set_sdpmlineindex(sdpMLineIndex);
    callback->set_allocated_on_cand(on_candidate);
  }

  void fillInStringMessage(librtcdcpp::Callback* callback, std::string message) {
    librtcdcpp::Callback::onMessage* on_message = new librtcdcpp::Callback::onMessage;
    on_message->set_message(message);
    callback->set_allocated_on_msg(on_message);
  }

  void SetCallbackTypeOnChannel(librtcdcpp::Callback* callback) {
    callback->set_cbwo_args(librtcdcpp::Callback::ON_CHANNEL);
  }

  void SetCallbackTypeOnClose(librtcdcpp::Callback* callback) {
    callback->set_cbwo_args(librtcdcpp::Callback::ON_CLOSE);
  }

  cb_event_loop* init_cb_event_loop() {
    printf("\nParent PID is: %d\n", getpid());
    cb_event_loop* cb_loop = new cb_event_loop();
    return cb_loop;
  }

  pc_info newPeerConnection(RTCConfiguration_C config_c, on_ice_cb ice_cb, dc_fn_ptr_pid dc_cb, cb_event_loop* parent_event_loop) {
    //spdlog::set_level(spdlog::level::info);
    //spdlog::set_pattern("*** [%H:%M:%S] %P %v ***");
    
    // Local callback that is registered to 'route' the callback to the event loop socket
    std::function<void(rtcdcpp::PeerConnection::IceCandidate)> onLocalIceCandidate = [ice_cb](rtcdcpp::PeerConnection::IceCandidate candidate)
    {
      IceCandidate_C ice_cand_c;
      ice_cand_c.candidate = candidate.candidate.c_str(); //
      ice_cand_c.sdpMid = candidate.sdpMid.c_str();
      ice_cand_c.sdpMLineIndex = candidate.sdpMLineIndex;
      librtcdcpp::Callback* callback = new librtcdcpp::Callback;
      fillInCandidate(callback, ice_cand_c.candidate, ice_cand_c.sdpMid, ice_cand_c.sdpMLineIndex);
      std::string serialized_cb;
      callback->SerializeToString(&serialized_cb);
      void *child_to_parent_context = zmq_ctx_new ();
      void *pusher = zmq_socket (child_to_parent_context, ZMQ_PUSH);
      char cb_connect_path[33];
      snprintf(cb_connect_path, sizeof(cb_connect_path), "ipc:///tmp/librtcdcpp%d-cb", getpid());
      int cb_rc = zmq_connect (pusher, cb_connect_path);
      zmq_send (pusher, (const void *) serialized_cb.c_str(), serialized_cb.size(), 0);
      zmq_close (pusher);
    };

    rtcdcpp::RTCConfiguration config;
    for(int i = 0; i < config_c.ice_servers->len; i++) {
      config.ice_servers.emplace_back(rtcdcpp::RTCIceServer{"stun3.l.google.com", 19302});
    }
    std::pair<unsigned, unsigned> port_range = std::make_pair(config_c.ice_port_range1, config_c.ice_port_range2);
    config.ice_port_range = port_range;

    if (config_c.ice_ufrag) {
      std::string ice_ufrag_string (config_c.ice_ufrag);
      config.ice_ufrag = ice_ufrag_string;
    }
    if (config_c.ice_pwd) {
      std::string ice_pwd_string (config_c.ice_pwd);
      config.ice_pwd = ice_pwd_string;
    }
    //TODO: Check this later. Doesn't seem to break any use-cases I'm aware of.
    /*
       rtcdcpp::RTCCertificate *rtc_cert;
       rtc_cert = (rtcdcpp::RTCCertificate*) malloc(sizeof(rtcdcpp::RTCCertificate));
       for(int i = 0; i <= config_c.certificates->len; i++) {
       rtc_cert = &g_array_index(config_c.certificates, rtcdcpp::RTCCertificate, i);
       config.certificates.emplace_back(&rtc_cert);
       }
    */ 

    pid_t cpid = fork();
    //TODO: Try using some options from SETSOCKOPT
    void *context = zmq_ctx_new ();
    void *requester = zmq_socket (context, ZMQ_REQ);

    void *cb_context = zmq_ctx_new ();
    void *cb_pull_socket = zmq_socket (cb_context, ZMQ_PULL);

    pc_info pc_info_ret;
    int process_status_var;
    process_status.push_front(process_status_var);

    if (cpid == 0) {
      //child
      DataChannel* child_dc;
      std::function<void(std::shared_ptr<DataChannel> channel, void* requester)> onDataChannel = [dc_cb, &requester, &child_dc](std::shared_ptr<DataChannel> channel, void* requester) {
        void *child_to_parent_context = zmq_ctx_new ();
        // TODO: onBinary and onError callbacks (later)
        std::function<void(std::string string1)> onStringMsg = [child_to_parent_context](std::string string1) {
          librtcdcpp::Callback* callback = new librtcdcpp::Callback;
          fillInStringMessage(callback, string1);
          std::string serialized_cb;
          callback->SerializeToString(&serialized_cb);
          void *pusher = zmq_socket (child_to_parent_context, ZMQ_PUSH);
          char cb_connect_path[33];
          snprintf(cb_connect_path, sizeof(cb_connect_path), "ipc:///tmp/librtcdcpp%d-cb", getpid());
          int cb_rc = zmq_connect (pusher, cb_connect_path);
          zmq_send (pusher, (const void *) serialized_cb.c_str(), serialized_cb.size(), 0);
          zmq_close (pusher); 
        };

        std::function<void()> onClosed = [child_to_parent_context]() {
          librtcdcpp::Callback* callback = new librtcdcpp::Callback;
          SetCallbackTypeOnClose(callback);
          std::string serialized_cb;
          callback->SerializeToString(&serialized_cb);
          void *pusher = zmq_socket (child_to_parent_context, ZMQ_PUSH);
          char cb_connect_path[33];
          snprintf(cb_connect_path, sizeof(cb_connect_path), "ipc:///tmp/librtcdcpp%d-cb", getpid());
          int cb_rc = zmq_connect (pusher, cb_connect_path);
          zmq_send (pusher, (const void *) serialized_cb.c_str(), serialized_cb.size(), 0);
          //zmq_setsockopt(pusher,  //no linger?
          if (zmq_close (pusher) != 0) {
            //printf("\nZMQ close error: %s\n", strerror(errno));
          }
          if (zmq_term(child_to_parent_context) != 0) {
            //printf("\nZMQ term error: %s\n", strerror(errno));
          }
          std::thread ext(exitter, 0, getpid());
          ext.detach();
        };
        
        child_dc = (DataChannel *) channel.get();
        child_dc->SetOnClosedCallback(onClosed);
        child_dc->SetOnStringMsgCallback(onStringMsg);

        librtcdcpp::Callback* callback = new librtcdcpp::Callback;
        SetCallbackTypeOnChannel(callback);
        std::string serialized_cb;
        if (callback->SerializeToString(&serialized_cb) == false) {
          printf("\nDC cb serialize error!\n");
          // return?
        }
        void *pusher = zmq_socket (child_to_parent_context, ZMQ_PUSH);
        char cb_connect_path[33];
        snprintf(cb_connect_path, sizeof(cb_connect_path), "ipc:///tmp/librtcdcpp%d-cb", getpid());
        int cb_rc = zmq_connect (pusher, cb_connect_path);
        zmq_send (pusher, (const void *) serialized_cb.c_str(), serialized_cb.size(), 0);
        zmq_close (pusher);
      };

      PeerConnection* child_pc;
      child_pc = new rtcdcpp::PeerConnection(config, onLocalIceCandidate, onDataChannel, requester); //Passing requester here is invalid
      void *child_context = zmq_ctx_new ();
      void *responder = zmq_socket (child_context, ZMQ_REP);
      char bind_path[30];
      snprintf(bind_path, sizeof(bind_path), "ipc:///tmp/librtcdcpp%d", getpid());
      int rc1 = zmq_bind (responder, bind_path);
      //printf("\nCreated file %s\n", bind_path);

      assert (rc1 == 0);
      bool alive = true;
      int command;

      while(alive) {
        zmq_recv (responder, &command, sizeof(command), 0);
        //printf("\nReceived command %d in process %d\n", command, getpid());
        switch(command) {
          case DESTROY_PC:
            sendSignal(responder); // Respond.
            _destroyPeerConnection(child_pc);
            break;
          case PARSE_SDP:
            {
              // send dummy response on command socket. This means it can start sending arguments
              sendSignal(responder);
              // Wait for arg. (which is length)
              size_t length;
              zmq_recv (responder, &length, sizeof(length), 0); // Wait for response
              // Ask for actual content in our response using dummy signal
              sendSignal(responder);
              char parse_sdp_arg[length];
              zmq_recv (responder, parse_sdp_arg, length, 0);
              _ParseOffer(child_pc, parse_sdp_arg);
              sendSignal(responder);
            }
            break;
          case GENERATE_OFFER:
            {
              char* offer;
              offer = _GenerateOffer(child_pc);
              size_t length = strlen(offer);
              zmq_send (responder, &length, sizeof(length), 0); // Respond with length of generate offer
              signalSink(responder); // Wait for next dummy REQ to send the content
              zmq_send (responder, offer, length, 0); // Respond with content
            }
            break;
          case GENERATE_ANSWER:
            {
              char* answer;
              answer = _GenerateAnswer(child_pc);
              size_t length = strlen(answer);
              zmq_send (responder, &length, sizeof(length), 0);
              signalSink(responder);
              zmq_send (responder, answer, length, 0);
            }
            break;
          case SET_REMOTE_ICE_CAND:
            {
              bool ret_bool;
              sendSignal(responder); // Respond with dummy signal to get length
              size_t length;
              zmq_recv (responder, &length, sizeof(length), 0);
              sendSignal(responder); // Respond with dummy signal to get content
              char candidate_sdp_arg[length];
              zmq_recv (responder, candidate_sdp_arg, length, 0);
              ret_bool = _SetRemoteIceCandidate(child_pc, candidate_sdp_arg);
              zmq_send (responder, &ret_bool, sizeof(ret_bool), 0); // Respond with return value
            }
            break;
          case SET_REMOTE_ICE_CANDS:
            {
              GArray *args_garray; //GArray
              // !
              // Send signal to send GArray
              sendSignal(responder);
              GArray *candidate_sdps_arg;
              candidate_sdps_arg = (GArray *) malloc(sizeof(GArray));
              zmq_recv (responder, candidate_sdps_arg, sizeof(candidate_sdps_arg), 0);
              bool ret_bool = _SetRemoteIceCandidates(child_pc, candidate_sdps_arg);
              signalSink(responder);
              zmq_send (responder, &ret_bool, sizeof(ret_bool), 0);
            }
            break;
          case CREATE_DC:
            {
            sendSignal(responder);
            // Get label length
            size_t label_arg_length;
            size_t proto_arg_length;
            zmq_recv(responder, &label_arg_length, sizeof(label_arg_length), 0);
            sendSignal(responder); // req proto length
            zmq_recv(responder, &proto_arg_length, sizeof(proto_arg_length), 0);
            sendSignal(responder); // req label
            char* label_arg = (char*) malloc(label_arg_length);
            char* proto_arg = (char*) malloc(proto_arg_length);
            zmq_recv(responder, label_arg, label_arg_length, 0);
            (label_arg)[label_arg_length] = '\0';
            sendSignal(responder); // req proto
            zmq_recv(responder, proto_arg, proto_arg_length, 0);
            (proto_arg)[proto_arg_length] = '\0';
            child_dc = (DataChannel *) malloc(sizeof(DataChannel *));
            child_dc = _CreateDataChannel(child_pc, label_arg, proto_arg);
            int pid = getpid();
            zmq_send(responder, &pid, sizeof(pid), 0); 
            }
            break;
          case CLOSE_DC:
            _closeDataChannel(child_dc);
            sendSignal(responder);
            alive = false; //!
            break;
          case GET_DC_SID:
            u_int16_t sid;
            sid = _getDataChannelStreamID(child_dc);
            zmq_send(responder, &sid, sizeof(sid), 0);
            break;
          case GET_DC_TYPE:
            u_int8_t dc_type;
            dc_type = _getDataChannelType(child_dc);
            zmq_send(responder, &dc_type, sizeof(dc_type), 0);
            break;
          case GET_DC_LABEL:
            {
            const char* chan_label;
            chan_label = _getDataChannelLabel(child_dc);
            size_t label_len = strlen(chan_label);
            zmq_send (responder, &label_len, sizeof(label_len), 0);
            signalSink(responder);
            zmq_send (responder, chan_label, label_len, 0);
            }
            break;
          case GET_DC_PROTO:
            {
            const char* chan_proto;
            chan_proto = _getDataChannelProtocol(child_dc);
            size_t proto_len = strlen(chan_proto);
            zmq_send (responder, &proto_len, sizeof(proto_len), 0);
            signalSink(responder);
            zmq_send (responder, chan_proto, proto_len, 0);
            }
            break;
          case SEND_STRING:
            {
            sendSignal(responder);
            size_t send_len;
            zmq_recv (responder, &send_len, sizeof(send_len), 0);
            char send_str[send_len];
            send_str[send_len] = '\0';
            sendSignal(responder);
            zmq_recv (responder, send_str, send_len, 0);
            bool ret_bool = _SendString(child_dc, send_str);
            zmq_send (responder, &ret_bool, sizeof(bool), 0);
            }
            break;
          case SEND_BINARY:
            // !
            {
            u_int8_t len;
            sendSignal(responder); //req length
            zmq_recv (responder, &len, sizeof(len), 0);
            u_int8_t send_stuff[len];
            sendSignal(responder);
            zmq_recv (responder, send_stuff, len, 0);
            bool ret_bool = _SendBinary(child_dc, send_stuff, len);
            zmq_send (responder, &ret_bool, sizeof(bool), 0);
            }
            break;
          default:
            break;
        }
      }
    } else {
      // Parent
      char cb_bind_path[33];
      snprintf(cb_bind_path, sizeof(cb_bind_path), "ipc:///tmp/librtcdcpp%d-cb", cpid);
      zmq_connect (cb_pull_socket, cb_bind_path);
      zmq_bind (cb_pull_socket, cb_bind_path);
      //printf("\nCreated file %s\n", cb_bind_path);
      parent_event_loop->addSocket(cpid, requester);
      parent_event_loop->add_pull_socket(cpid, cb_pull_socket);
      parent_event_loop->add_on_candidate(cpid, ice_cb);
      parent_event_loop->add_on_datachannel(cpid, dc_cb);
      char connect_path[30];
      snprintf(connect_path, sizeof(connect_path), "ipc:///tmp/librtcdcpp%d", cpid);
      int rc2 = zmq_connect(requester, connect_path);
      //assert (rc2 == 0);
      pc_info_ret.socket = requester;
      pc_info_ret.pid = cpid;
    }
    return pc_info_ret;
  }

  void _waitCallable(int i) {
    pid_t process_id;
    process_id = wait(&i);
    if (process_id == -1) {
      //printf("\nWait err: %s\n", strerror(errno));
      return;
    }
    printf("\nProcess %d has terminated with status code %d\n", process_id, i);
    if (WIFEXITED(i)) {
      printf("\nIt exited normally with status code %d\n", WEXITSTATUS(i));
    }
    if (WIFSIGNALED(i)) {
      printf("\nIt exited by signal with sig no %d\n", WTERMSIG(i));
      if (WCOREDUMP(i)) {
        printf("\nIt exited with a coredump.\n");
      }
    }
    if (WIFSTOPPED(i)) {
      printf("\nProcess stopped by a trace signal %d\n", WSTOPSIG(i));
    }
    if (WIFCONTINUED(i)) {
      printf("\nProcess resumed by SIGCONT signal\n");
    }
  }

  void processWait() {
    for (int i : process_status) {
      _waitCallable(std::ref(i)); //!
    }
    sleep(3); //Maybe a little overkill or not needed at all. Run some tests again
  }

  void exitter(pid_t ret, int pid) {
    char tmp_path[30];
    sleep(1); // During onClose, to let the other peer reach this stage. (This is supposed to be parent context)
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/librtcdcpp%d", getpid());
    //printf("\nRemoving %s\n", tmp_path);
    if (remove(tmp_path) == -1) {
      printf("\nRemoving %s error: %s\n", tmp_path, strerror(errno));
    }
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/librtcdcpp%d-cb", getpid());
    //printf("\nRemoving %s\n", tmp_path);
    if (remove(tmp_path) == -1) {
      printf("\nRemoving %s error: %s\n", tmp_path, strerror(errno));
    }
    kill(pid, SIGTERM); //stop child process
    exit(ret);
  }

  void _destroyPeerConnection(PeerConnection *pc) {
    delete pc;
  }

  void _ParseOffer(PeerConnection* pc, const char* sdp) {
    std::string sdp_string (sdp);
    pc->ParseOffer(sdp_string);
  }

  void destroyPeerConnection(void *socket) {
    int command = DESTROY_PC;
    zmq_send (socket, &command, sizeof(command), 0);
    signalSink(socket);
  }

  void ParseOffer(void *socket, const char *sdp) {
    int child_command = PARSE_SDP;
    zmq_send (socket, &child_command, sizeof(child_command), 0); // Send command
    size_t sdp_length = strlen(sdp);
    signalSink(socket);
    zmq_send (socket, &sdp_length, sizeof(sdp_length), 0);
    signalSink(socket);
    zmq_send (socket, sdp, sdp_length, 0);
    signalSink(socket);
  }

  char* GenerateOffer(void* socket) {
    int child_command = GENERATE_OFFER;
    zmq_send (socket, &child_command, sizeof(child_command), 0); // Send command request
    // Response will contain length of generated offer
    size_t* length = (size_t *) malloc(sizeof(size_t));
    if (zmq_recv (socket, length, sizeof(length), 0) == -1) {
      //printf("\nGenerate offer length receive error: %d\n", errno);
      return NULL;
    }
    sendSignal(socket); // dummy request for content
    char* recv_offer = (char*) malloc(*length);
    recv_offer[*length] = '\0';
    zmq_recv (socket, recv_offer, *length, 0);
    free(length);
    return recv_offer;
  }

  char* GenerateAnswer(void *socket) {
    int command = GENERATE_ANSWER;
    zmq_send (socket, &command, sizeof(command), 0);
    size_t length;
    if (zmq_recv (socket, &length, sizeof(length), 0) == -1) {
      //printf("\nGenerate answer length receive error: %d\n", errno);
      return NULL;
    }
    sendSignal(socket);
    char *answer = (char *) malloc(length);
    zmq_recv (socket, answer, length, 0);
    answer[length] = '\0';
    return answer;
  }

  bool SetRemoteIceCandidate(void *socket, const char* candidate_sdp) {
    int command = SET_REMOTE_ICE_CAND;
    zmq_send (socket, &command, sizeof(command), 0);
    signalSink(socket);
    size_t candidate_sdp_len = strlen(candidate_sdp);
    zmq_send (socket, &candidate_sdp_len, sizeof(candidate_sdp_len), 0);
    signalSink(socket);
    // send content
    zmq_send (socket, candidate_sdp, candidate_sdp_len, 0);
    // Get response that contains return boolean
    bool ret_val;
    zmq_recv (socket, &ret_val, sizeof(ret_val), 0);
    return ret_val;
  }

  bool SetRemoteIceCandidates(void *socket, const GArray* candidate_sdps) {
    //return false; // WIP
    int command = SET_REMOTE_ICE_CANDS;
    zmq_send (socket, &command, sizeof(command), 0); // Send command request
    signalSink(socket);
    zmq_send (socket, candidate_sdps, sizeof(candidate_sdps), 0);
    bool ret_val;
    zmq_recv (socket, &ret_val, sizeof(ret_val), 0);
    return ret_val;
  }

  int CreateDataChannel(void* socket, const char* label, const char* protocol) {
    int command = CREATE_DC;
    zmq_send (socket, &command, sizeof(command), 0);
    signalSink(socket);
    // send lengths
    size_t label_length = strlen(label);
    size_t protocol_length = strlen(protocol);
    zmq_send (socket, &label_length, sizeof(label_length), 0);
    signalSink(socket);
    zmq_send (socket, &protocol_length, sizeof(protocol_length), 0);
    signalSink(socket);
    zmq_send (socket, label, label_length, 0);
    signalSink(socket);
    zmq_send (socket, protocol, protocol_length, 0);
    int pid;
    zmq_recv (socket, &pid, sizeof(pid), 0);
    return pid;
  };
  
  void closeDataChannel(void* socket) {
    int command = CLOSE_DC;
    zmq_send (socket, &command, sizeof(command), 0);
    signalSink(socket);
  }

  u_int16_t getDataChannelStreamID(void* socket, DataChannel* dc) {
    int command = GET_DC_SID;
    u_int16_t sid;
    zmq_send (socket, &command, sizeof(command), 0);
    zmq_recv (socket, &sid, sizeof(sid), 0);
    return sid;
  }


  u_int8_t getDataChannelType(void* socket, DataChannel *dc) {
    u_int8_t dc_type;
    int command = GET_DC_TYPE;
    zmq_send (socket, &command, sizeof(command), 0);
    zmq_recv (socket, &dc_type, sizeof(dc_type), 0);
    return dc_type;
  }

  const char* getDataChannelLabel(void* socket, DataChannel *dc) {
    int command = GET_DC_LABEL;
    zmq_send (socket, &command, sizeof(command), 0);
    size_t label_len;
    zmq_recv (socket, &label_len, sizeof(label_len), 0);
    sendSignal(socket);
    char label[label_len];
    zmq_recv (socket, label, label_len, 0);
    char* label_ptr = label;
    return label_ptr;
  }

  const char* getDataChannelProtocol(void *socket, DataChannel *dc) {
    int command = GET_DC_PROTO;
    zmq_send (socket, &command, sizeof(command), 0);
    size_t proto_len;
    zmq_recv (socket, &proto_len, sizeof(proto_len), 0);
    sendSignal(socket);
    char proto[proto_len];
    zmq_recv (socket, proto, proto_len, 0);
    char* proto_ptr = proto;
    return proto_ptr;
  }


  bool SendString(void* socket, const char* msg) {
    int command = SEND_STRING;
    zmq_send (socket, &command, sizeof(command), 0);
    signalSink(socket);
    size_t send_len = strlen(msg);
    // Send length of our msg
    zmq_send (socket, &send_len, sizeof(send_len), 0);
    signalSink(socket);
    zmq_send (socket, msg, send_len, 0);
    bool ret_val;
    zmq_recv (socket, &ret_val, sizeof(ret_val), 0);
    return ret_val;
  }
  
  bool SendBinary(void* socket, DataChannel *dc, const u_int8_t *msg, int len) {
    int command = SEND_BINARY;
    zmq_send (socket, &command, sizeof(command), 0);
    signalSink(socket);
    // Send length of our msg
    zmq_send (socket, &len, sizeof(len), 0);
    signalSink(socket);
    zmq_send (socket, msg, len, 0);
    bool ret_val;
    zmq_recv (socket, &ret_val, sizeof(ret_val), 0);
    return ret_val;
  }


  void SetOnOpen(int pid, cb_event_loop* cb_event_loop, open_cb on_open_cb) {
    cb_event_loop->add_on_open(pid, on_open_cb);
  }

  void SetOnStringMsgCallback(int pid, cb_event_loop* cb_event_loop, on_string_msg recv_str_cb) {
    cb_event_loop->add_on_string(pid, recv_str_cb);
  }

  void SetOnBinaryMsgCallback(void *socket, on_binary_msg msg_binary_cb) {
    //TODO: SetOnBinaryMsgCallback
  }

  void SetOnErrorCallback(void *socket, DataChannel *dc, on_error error_cb) {
    //TODO: SetOnErrorCallback
  }

  void SetOnClosedCallback(int pid, cb_event_loop* cb_event_loop, on_close close_cb) {
    cb_event_loop->add_on_close(pid, close_cb);
  }

  char* _GenerateOffer(PeerConnection *pc) {
    std::string ret_val;
    ret_val = pc->GenerateOffer();
    char* ret_val1 = (char*) malloc(ret_val.size());
    snprintf(ret_val1, ret_val.size(), ret_val.c_str());
    return ret_val1;
  }

  char* _GenerateAnswer(PeerConnection *pc) {
    std::string ret_val;
    ret_val = pc->GenerateAnswer();
    char* ret_val1 = (char*) malloc(ret_val.size());
    snprintf(ret_val1, ret_val.size(), ret_val.c_str());
    return ret_val1;
  }

  bool _SetRemoteIceCandidate(PeerConnection *pc, const char* candidate_sdp) {
    std::string candidate_sdp_string (candidate_sdp);
    return pc->SetRemoteIceCandidate(candidate_sdp_string);
  }

  bool _SetRemoteIceCandidates(PeerConnection *pc, const GArray* candidate_sdps) {
    std::vector<std::string> candidate_sdps_vec;
    for(int i = 0; i <= candidate_sdps->len; i++) {
      std::string candidate_sdp_string (&g_array_index(candidate_sdps, char, i));
      candidate_sdps_vec.emplace_back(candidate_sdp_string);
    }
    return pc->SetRemoteIceCandidates(candidate_sdps_vec);
  }

  DataChannel* _CreateDataChannel(PeerConnection *pc, char* label, char* protocol) {
    std::string label_string (label);
    std::string protocol_string (protocol);
    free(label); free(protocol);
    if (protocol_string.size() > 0) {
      return pc->CreateDataChannel(label_string, protocol_string).get();
    } else {
      return pc->CreateDataChannel(label_string).get();
    }
  }

  u_int16_t _getDataChannelStreamID(DataChannel *dc) {
    return dc->GetStreamID();
  }

  u_int8_t _getDataChannelType(DataChannel *dc) {
    return dc->GetChannelType();
  }

  const char* _getDataChannelLabel(DataChannel *dc) {
    std::string dc_label_string;
    dc_label_string = dc->GetLabel();
    return dc_label_string.c_str();
  }

  const char* _getDataChannelProtocol(DataChannel *dc) {
    std::string dc_proto_string;
    dc_proto_string = dc->GetProtocol();
    return dc_proto_string.c_str();
  }

  bool _SendString(DataChannel *dc, const char* msg) {
    std::string message (msg);
    return dc->SendString(message);
  }

  bool _SendBinary(DataChannel *dc, u_int8_t *msg, int len) {
    return dc->SendBinary(msg, len);
  }

  void _closeDataChannel(DataChannel *dc) {
    dc->Close();
  }
}
