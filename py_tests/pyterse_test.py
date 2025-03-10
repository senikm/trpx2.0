import unittest
import numpy as np
import io
import shutil
import sys
import os

for root, dirs, files in os.walk('.'):
    if '__pycache__' in dirs:
        shutil.rmtree(os.path.join(root, '__pycache__'))

sys.path.append(os.path.join(os.getcwd(), 'build', 'pyterse/'))

from pyterse import Terse, TerseMode

class TestTerseLibrary(unittest.TestCase):
    def setUp(self):
        # Common test data
        self.test_data_1d = np.array([1, 2, 3, 4, 5], dtype=np.int32)
        self.test_data_2d = np.array([[1, 2, 3], [4, 5, 6]], dtype=np.int32)
        self.test_data_3d = np.array([[[1, 2], [3, 4]], [[5, 6], [7, 8]]], dtype=np.int32)

    def test_basic_construction(self):
        """Test basic Terse object construction"""
        terse = Terse()
        self.assertEqual(terse.number_of_frames, 0)
        self.assertEqual(terse.size, 0)

    def test_construction_with_data(self):
        """Test Terse construction with initial data"""
        terse = Terse(self.test_data_1d, TerseMode.SIGNED)
        self.assertEqual(terse.size, len(self.test_data_1d))
        self.assertTrue(terse.is_signed)

    def test_data_types(self):
        """Test different numpy data types"""
        for dtype in [np.int8, np.uint8, np.int16, np.uint16, np.int32, np.uint32, np.int64, np.uint64]:
            data = np.array([1, 2, 3], dtype=dtype)
            terse = Terse(data, TerseMode.SIGNED)
            result = terse.prolix()
            np.testing.assert_array_equal(result, data)
            self.assertEqual(result.dtype, data.dtype)


    def test_dimensions(self):
        """Test handling of different dimensional arrays"""
        # 1D
        terse_1d = Terse(self.test_data_1d)
        np.testing.assert_array_equal(terse_1d.prolix(), self.test_data_1d)
        
        # 2D
        terse_2d = Terse(self.test_data_2d)
        np.testing.assert_array_equal(terse_2d.prolix(), self.test_data_2d)
        
        # 3D
        terse_3d = Terse(self.test_data_3d)
        np.testing.assert_array_equal(terse_3d.prolix(), self.test_data_3d)

    def test_compression_modes(self):
        """Test different compression modes"""
        data = np.array([1, 2, 3], dtype=np.int32)
        for mode in [TerseMode.SIGNED, TerseMode.UNSIGNED, TerseMode.SMALL_UNSIGNED, TerseMode.DEFAULT]:
            terse = Terse(data, mode)
            np.testing.assert_array_equal(terse.prolix(), data)

    def test_frame_operations(self):
        """Test frame-based operations"""
        # Insert frames
        terse = Terse()
        terse.push_back(self.test_data_1d)
        terse.insert(0, self.test_data_1d)
        self.assertEqual(terse.number_of_frames, 2)
        
        # Access frame
        frame = terse.at(0)
        np.testing.assert_array_equal(frame.prolix(), self.test_data_1d)
        
        # Erase frame
        terse.erase(0)
        self.assertEqual(terse.number_of_frames, 1)

    def test_metadata(self):
        """Test metadata operations"""
        terse = Terse(self.test_data_1d)
        test_metadata = "<metadata>test</metadata>"
        terse.set_metadata(0, test_metadata)
        self.assertEqual(terse.metadata(0), test_metadata)

    def test_dimension_operations(self):
        """Test dimension-related operations"""
        terse = Terse(self.test_data_2d)
        original_dims = terse.dim()
        
        # Test dimension setting
        new_dims = [6, 1]  # Same total size as 2x3
        terse.set_dim(new_dims)
        self.assertEqual(terse.dim(), new_dims)
        
        # Test invalid dimension setting
        with self.assertRaises(ValueError):
            terse.set_dim([2, 2])  # Invalid total size

    def test_stream_operations(self):
        """Test stream reading and writing"""
        # Write to stream
        stream = io.BytesIO()
        terse_write = Terse(self.test_data_1d)
        terse_write.write(stream)
        
        # Read from stream
        stream.seek(0)
        terse_read = Terse(stream)
        np.testing.assert_array_equal(terse_read.prolix(), self.test_data_1d)

    def test_compression_settings(self):
        """Test compression settings"""
        terse = Terse()
        
        # Test block size
        terse.set_block_size(1024)
        self.assertEqual(terse.block_size(), 1024)
        
        # Test fast mode
        terse.set_fast(True)
        self.assertTrue(terse.fast())
        
        # Test small mode
        terse.set_small(True)
        self.assertTrue(terse.small())
        
        # Test degree of parallelism
        terse.set_dop(0.5)
        self.assertEqual(terse.dop(), 0.5)

    def test_error_handling(self):
        """Test error handling"""
        terse = Terse(self.test_data_1d)
        
        # Test invalid frame access
        with self.assertRaises(IndexError):
            terse.at(999)
        
        # Test invalid frame insertion
        with self.assertRaises(ValueError):
            terse.insert(999, self.test_data_1d)
        
        # Test dimension mismatch
        different_shape = np.array([[1, 2], [3, 4]], dtype=np.int32)
        with self.assertRaises(ValueError):
            terse.push_back(different_shape)

    def test_memory_management(self):
        """Test memory management features"""
        terse = Terse(self.test_data_1d)
        original_size = terse.terse_size
        terse.shrink_to_fit()
        self.assertLessEqual(terse.terse_size, original_size)

if __name__ == '__main__':
    unittest.main(verbosity=2)