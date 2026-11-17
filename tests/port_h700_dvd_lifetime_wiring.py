"""Source binding for the CPU DVD lifetime regression; no game/device execution."""
import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from wiring_helpers import braced, squash


def source(path):
    text = (ROOT / path).read_text()
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


class H700DvdLifetimeWiring(unittest.TestCase):
    def body(self, path, signature):
        return braced(source(path), signature, path)

    def guarded_retirement(self, body, retirement):
        guarded = re.search(r"#ifdef AURORA_H700\s*(.*?)#endif", body, re.S)
        self.assertIsNotNone(guarded)
        self.assertEqual(squash(guarded.group(1)), squash(retirement))
        self.assertNotIn("IsComplete", body)
        self.assertNotIn("PostCancelRequest", body)
        self.assertNotIn("DVDCancelAll", body)
        self.assertNotIn("AsyncIdle", body)
        return guarded.end()

    def test_world_retires_before_explicit_destination_free(self):
        body = self.body("src/MetroidPrime/CWorld.cpp", "CWorld::~CWorld()")
        end = self.guarded_retirement(body, "x3c_loadToken = nullptr;")
        self.assertLess(end, body.index("CMemory::Free(x40_loadBuf.release())"))

    def test_dummy_retires_before_member_tail(self):
        body = self.body("src/MetroidPrime/CWorld.cpp", "CDummyWorld::~CDummyWorld()")
        self.guarded_retirement(body, "x30_loadToken = nullptr;")
        self.assertEqual(squash(re.sub(r"#ifdef AURORA_H700.*?#endif", "", body, flags=re.S)), "{}")

    def test_loading_data_retires_owned_request_before_zip_and_buffers(self):
        body = self.body("src/Kyoto/CResFactory.cpp", "CResFactory::SLoadingData::~SLoadingData()")
        end = self.guarded_retirement(body, "x8_dvdReq = rstl::auto_ptr< CDvdRequest >();")
        self.assertLess(end, body.index("inflateEnd"))
        # reset() drops ownership without deleting; delete get() deletes a borrowed
        # pointer after auto_ptr transfer. Neither is a valid retirement here.
        self.assertNotIn(".reset(", body)
        self.assertNotIn("delete", body)

    def test_real_request_uses_callback_draining_close_not_throwing_poll(self):
        body = self.body("src/Kyoto/CDvdRequest.cpp", "CRealDvdRequest::~CRealDvdRequest()")
        self.assertEqual(squash(body), "{DVDClose(&mFileInfo);}")

    def test_aram_waits_actual_dma_only_on_h700(self):
        cpp = source("src/Kyoto/CDvdRequest.cpp")
        header = source("include/Kyoto/CDvdRequest.hpp")
        self.assertRegex(cpp, r"#ifdef AURORA_H700\s*CARAMDvdRequest::~CARAMDvdRequest\(\)\s*\{\s*WaitUntilComplete\(\);\s*\}\s*#endif")
        self.assertRegex(header, r"#ifdef AURORA_H700\s*~CARAMDvdRequest\(\) override;\s*#endif")
        body = self.body("src/Kyoto/CDvdRequest.cpp", "void CARAMDvdRequest::WaitUntilComplete()")
        self.assertIn("CARAMManager::WaitForDMACompletion(x4_dmaReq)", body)
        self.assertNotIn("IsComplete", body)


if __name__ == "__main__":
    unittest.main()
