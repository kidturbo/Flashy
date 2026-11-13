# Replicate the workflow

Use these stages for your own project. A stage's output is evidence for its stated scope, not permission for the next live stage.

## 1. Establish your target privately

Copy templates/project.md into a private project directory. Record actual vehicle/engine/transmission, ECU hardware and software identities, adapter firmware/backend, physically connected host, reader revision, fuel/modifications/goals, existing faults and measurement availability. Do not post VIN, hardware serials or complete identity responses. Verify mechanical health before raising load/output; unresolved faults remain unresolved even when diagnosis is deferred.

## 2. Obtain and preserve originals

Use a reader supported and verified for your exact ECU/software. Record full/calibration/virtual read type, address coverage, byte count, substituted bytes, excluded shadow ranges, hashes and source revision. Obtain two matching reads and retain a verified off-host copy. See acquisition.md for the worked case and its boundaries. Avoid treating another owner's image as your backup.

## 3. Determine applicability

Calculate SHA-256 locally. This package's production tools require the exact worked baseline; a mismatch stops the workflow. An exact match allows reproducing these file calculations, not assuming hardware/vehicle equivalence. For a mismatch, use adapting.md to establish a new reviewed profile instead of editing the expected digest until a script accepts the file.

## 4. Reproduce the offline checks

Run the synthetic demo and public tests without a vehicle. For the exact baseline only, set E78_PRIVATE_BASELINE to the private input path. Follow tools/README.md to audit checksums, export the baseline worksheet, and verify the unchanged values. Use fresh ignored private directories; never modify an original in place.

## 5. Define one explicit change

Copy the exported CSV privately, edit proposed_kw only for intended cells, and provide a concrete reason for each changed cell. Record old/new values, units, rationale, dependent states and expected measurable effect using templates/change.md. The original table's numeric range is an input guard, not proof of calibration safety. The worked pedal example explains one experimental proposal without recommending it for another vehicle.

## 6. Build and independently review

Build to a fresh private directory. Verify output hashes, all 17 checks, exact cell and byte differences, protected metadata, physical changed blocks, selection/transition implications and unchanged regions. A reviewer receives a frozen artifact hash and the original proposal. Record actual commands/results in templates/review.md; keep flash_ready=false. Do not confuse a successful CSV roundtrip with validated interpolation or measured output.

## 7. Resolve programming and validation dependencies

Read tuning-and-validation.md. The open research core has simulated flash callbacks only. Establish a real supported writer, compatible file/container, target-specific recovery and power requirements before any physical write. Secure exact-operation authorization separately. Retain the original, candidate and plan hashes so that the reviewed bytes match any subsequently selected operation.

## 8. Contribute reproducible evidence

Publish sanitized source and synthetic examples, a precise profile/evidence ledger and negative findings. Keep private captures and firmware private. Use templates/session.md for conditions/limits and docs/publication.md for the release process. A new ECU/OS profile needs independent verification, not agreement among agents.
