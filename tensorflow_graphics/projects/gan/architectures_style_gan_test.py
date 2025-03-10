# Copyright 2020 The TensorFlow Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Tests for gan.architectures_style_gan."""

from absl.testing import parameterized
import numpy as np
import tensorflow as tf

from tensorflow_graphics.projects.gan import architectures_style_gan


class ArchitecturesStyleGanTest(tf.test.TestCase, parameterized.TestCase):

  @parameterized.named_parameters(('batch_1', 1, False), ('batch_2', 2, False),
                                  ('normalize_latent_code', 1, True))
  def test_style_based_generator_output_size(self, batch_size,
                                             normalize_latent_code):
    input_data = np.ones(shape=(batch_size, 8), dtype=np.float32)
    generator, _, _ = architectures_style_gan.create_style_based_generator(
        latent_code_dimension=8,
        upsampling_blocks_num_channels=(8, 8),
        normalize_latent_code=normalize_latent_code)
    expected_size = 16

    output = generator(input_data)
    output_value = self.evaluate(output)

    with self.subTest(name='static_shape'):
      output.shape.assert_is_fully_defined()
      self.assertSequenceEqual(output.shape,
                               (batch_size, expected_size, expected_size, 3))
    with self.subTest(name='dynamic_shape'):
      self.assertSequenceEqual(output_value.shape,
                               (batch_size, expected_size, expected_size, 3))

  @parameterized.named_parameters(('batch_1', 1), ('batch_2', 2))
  def test_style_based_generator_intermediate_outputs_shape(self, batch_size):
    input_data = tf.ones(shape=(batch_size, 8))
    generator, _, _ = architectures_style_gan.create_style_based_generator(
        latent_code_dimension=8,
        upsampling_blocks_num_channels=(8, 8),
        generate_intermediate_outputs=True)

    outputs = generator(input_data)
    output_values = self.evaluate(outputs)

    self.assertLen(outputs, 3)
    for index, output_value in enumerate(output_values):
      self.assertSequenceEqual(output_value.shape,
                               (batch_size, 2**(index + 2), 2**(index + 2), 3))

  def test_cloning_style_based_generator(self):
    generator, _, _ = architectures_style_gan.create_style_based_generator()

    with tf.keras.utils.custom_object_scope(
        architectures_style_gan.CUSTOM_LAYERS):
      generator_clone = tf.keras.models.clone_model(generator)

    self.assertIsInstance(generator_clone, tf.keras.Model)


if __name__ == '__main__':
  tf.test.main()
