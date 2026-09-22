/* SPDX-License-Identifier: Apache-2.0 */
package n3172tq

import "github.com/salvaged-silicon/nosaic-switch/internal/platformhal"

func init() {
	platformhal.Register("n3172tq", func(cfg platformhal.Config) (platformhal.HAL, error) {
		return Open(cfg)
	})
}
