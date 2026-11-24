// Build inside a checkout of syncthing/syncthing (copy to cmd/xstgolden/ and `go run ./cmd/xstgolden`);
// regenerates tests/golden/bep.txt. Upstream commit: see proto/UPSTREAM.md.
// Prints protobuf encodings of fixed BEP messages, as hex, for xsyncthing's golden tests.
package main

import (
	"encoding/hex"
	"fmt"

	"google.golang.org/protobuf/proto"

	"github.com/syncthing/syncthing/internal/gen/bep"
)

func out(name string, m proto.Message) {
	b, err := proto.MarshalOptions{Deterministic: true}.Marshal(m)
	if err != nil {
		panic(err)
	}
	fmt.Printf("%s %s\n", name, hex.EncodeToString(b))
}

func main() {
	id := make([]byte, 32)
	for i := range id {
		id[i] = byte(i + 1)
	}
	hash := make([]byte, 32)
	for i := range hash {
		hash[i] = byte(0xA0 + i)
	}
	out("hello", &bep.Hello{DeviceName: "xbox", ClientName: "xsyncthing", ClientVersion: "v0.0.1", NumConnections: 1, Timestamp: 1700000000123456789})
	out("cluster_config", &bep.ClusterConfig{Folders: []*bep.Folder{{
		Id: "abcd-1234", Label: "Saves", Type: bep.FolderType_FOLDER_TYPE_SEND_RECEIVE,
		Devices: []*bep.Device{{Id: id, Name: "pc", Addresses: []string{"dynamic", "tcp://192.168.1.2:22000"}, Compression: bep.Compression_COMPRESSION_METADATA, MaxSequence: 42, IndexId: 0x1122334455667788}},
	}}})
	out("index", &bep.Index{Folder: "abcd-1234", LastSequence: 7, Files: []*bep.FileInfo{{
		Name: "dir/file.txt", Type: bep.FileInfoType_FILE_INFO_TYPE_FILE, Size: 131072 + 5, Permissions: 0o644,
		ModifiedS: 1700000000, ModifiedNs: 123456789, ModifiedBy: 0x5A5A5A5A5A5A5A5A,
		Version: &bep.Vector{Counters: []*bep.Counter{{Id: 0x5A5A5A5A5A5A5A5A, Value: 3}}},
		Sequence: 7, BlockSize: 131072,
		Blocks: []*bep.BlockInfo{{Offset: 0, Size: 131072, Hash: hash}, {Offset: 131072, Size: 5, Hash: hash}},
		BlocksHash: hash,
	}, {
		Name: "gone", Deleted: true, Sequence: 6, Version: &bep.Vector{Counters: []*bep.Counter{{Id: 1, Value: 9}}},
	}}})
	out("index_update", &bep.IndexUpdate{Folder: "f", LastSequence: 9, PrevSequence: 7})
	out("request", &bep.Request{Id: 5, Folder: "abcd-1234", Name: "dir/file.txt", Offset: 131072, Size: 5, Hash: hash, BlockNo: 1})
	out("response", &bep.Response{Id: 5, Data: []byte("hello"), Code: bep.ErrorCode_ERROR_CODE_NO_ERROR})
	out("response_err", &bep.Response{Id: 6, Code: bep.ErrorCode_ERROR_CODE_NO_SUCH_FILE})
	out("download_progress", &bep.DownloadProgress{Folder: "f", Updates: []*bep.FileDownloadProgressUpdate{{Name: "a", BlockIndexes: []int32{0, 1, 200}, BlockSize: 131072, Version: &bep.Vector{Counters: []*bep.Counter{{Id: 1, Value: 2}}}}}})
	out("close", &bep.Close{Reason: "bye"})
	out("header_lz4", &bep.Header{Type: bep.MessageType_MESSAGE_TYPE_INDEX, Compression: bep.MessageCompression_MESSAGE_COMPRESSION_LZ4})
}
