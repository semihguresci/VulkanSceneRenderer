"""Post-run VMA ownership checks for validated #106 static/resource reports.

This never launches a renderer. Both reports must bind the same actual executable
SHA256, contain only validated captures, and retain unchanged artifact hashes.
Comparisons use matching source composition and retained asset-generation counts:
two imports around failed static requests, or merge/redo/restored Default within
one resource process. Initial borrowed buffers are intentionally not compared.

Equal VMA allocation counts/bytes show no net VMA growth at these settled states.
They do not establish driver-object, descriptor-pool or total-memory leak freedom.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import tempfile


UINT64_MAX = (1 << 64)-1
STATIC_STATES = ('transaction-redo2', 'transaction-missing', 'transaction-malformed',
                 'transaction-capacity', 'transaction-redo2-final')
RESOURCE_CASES = ('gui-progress', 'ray-ddgi', 'frame-rebuild', 'first-extended')
VALIDATION_ERROR = re.compile(r'VUID-|SYNC-HAZARD|Validation Error|\bhazard detected\b|'
    r'\b(?:READ|WRITE)_AFTER_(?:READ|WRITE)\b[^\r\n]*\bhazard\b', re.I)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def unsigned(value, label):
    require(type(value) is int and 0 <= value <= UINT64_MAX, 'Missing/invalid uint64: '+label)
    return value


def finite(value):
    if isinstance(value, float): return math.isfinite(value)
    if isinstance(value, dict): return all(finite(child) for child in value.values())
    if isinstance(value, list): return all(finite(child) for child in value)
    return True


def load_json(path):
    document = json.loads(path.read_text(encoding='utf-8-sig'))
    require(isinstance(document, dict) and finite(document), 'Nonfinite/nonobject JSON: '+str(path))
    return document


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(1024*1024): digest.update(block)
    return digest.hexdigest()


def resolve(path, root):
    require(isinstance(path, str) and path, 'Missing artifact path')
    result = Path(path)
    return (result if result.is_absolute() else root/result).resolve()


def check_file(path, expected, verified):
    require(isinstance(expected, str) and re.fullmatch(r'[0-9a-fA-F]{64}', expected), 'Invalid recorded SHA256: '+str(path))
    expected = expected.lower()
    require(path.is_file() and sha(path) == expected, 'Recorded artifact SHA256 mismatch: '+str(path))
    key = str(path.resolve())
    require(key not in verified or verified[key] == expected, 'Conflicting recorded artifact hashes: '+key)
    verified[key] = expected


def manifest_proof(before, after, root, renderer, renderer_sha, verified):
    require(isinstance(before, dict) and isinstance(after, dict) and before.get('verified') is True and after.get('verified') is True,
            'Missing verified before/after native manifest')
    require(before.get('sha256') == after.get('sha256') and before.get('entryCount') == after.get('entryCount') and
            resolve(before.get('path'), root) == resolve(after.get('path'), root), 'Native manifest changed during validation')
    path = resolve(before['path'], root)
    check_file(path, before['sha256'], verified)
    files = load_json(path).get('files')
    require(isinstance(files, dict) and files and len(files) == unsigned(before.get('entryCount'), 'manifest entries'),
            'Frozen manifest file table/count differs')
    renderer_found = False
    for name, digest in files.items():
        item = resolve(name, path.parent)
        check_file(item, digest, verified)
        if item == renderer:
            renderer_found = digest.lower() == renderer_sha
    require(renderer_found, 'Manifest does not bind the report renderer SHA256')


def allocation_summary(state):
    merge = state.get('gltfMerge')
    require(isinstance(merge, dict), 'Missing actual gltfMerge telemetry')
    return dict(count=unsigned(merge.get('vmaAllocationCount'), 'gltfMerge.vmaAllocationCount'),
        bytes=unsigned(merge.get('vmaAllocationBytes'), 'gltfMerge.vmaAllocationBytes'),
        retained=unsigned(merge.get('retainedGenerationCount'), 'gltfMerge.retainedGenerationCount'))


def frame_shape(state):
    extent = state.get('resolution')
    require(isinstance(extent, list) and len(extent) == 2 and
            all(unsigned(component, 'resolution') > 0 for component in extent), 'Invalid frame extent')
    images = unsigned(state.get('swapchainImages'), 'swapchainImages')
    require(images > 0, 'Empty swapchain frame table')
    return (images, tuple(extent))


def compare_allocations(first, second, label, retained, *, allow_frame_shape_skip=False):
    a, b = allocation_summary(first), allocation_summary(second)
    require(a['retained'] == b['retained'] == retained, 'Different retained asset-generation counts: '+label)
    require(first['gltfMerge']['sourceTables'] == second['gltfMerge']['sourceTables'] and
            first['gltfScenes']['graphIdentity'] == second['gltfScenes']['graphIdentity'] and
            first['gltfScenes']['selected'] == second['gltfScenes']['selected'] and
            first['scene']['primaryObjectCount'] == second['scene']['primaryObjectCount'],
            'Different source composition/view for a VMA comparison: '+label)
    shape_a, shape_b = frame_shape(first), frame_shape(second)
    if shape_a != shape_b:
        require(allow_frame_shape_skip, 'Unexpected different frame resource shape: '+label)
        return dict(name=label, comparisonPerformed=False, reason='Swapchain image count or extent differs; VMA equality is not asserted',
            firstFrameShape=shape_a, secondFrameShape=shape_b, firstVma=a, secondVma=b)
    require(a['count'] == b['count'] and a['bytes'] == b['bytes'], 'Settled VMA allocation count/bytes changed: '+label+
        '; before='+str(a)+'; after='+str(b))
    return dict(name=label, comparisonPerformed=True, passed=True, vmaAllocationCount=a['count'],
        vmaAllocationBytes=a['bytes'], retainedGenerationCount=retained, frameShape=shape_a)


def checked_report(path, kind, verified):
    path = path.resolve(); document = load_json(path); root = path.parent
    check_file(path, sha(path), verified)
    require(document.get('passed') is True and document.get('generatedOnly') is False and
            document.get('nativeValidationPerformed') is True, 'Report is not a completed native pass: '+str(path))
    require(unsigned(document.get('excludedDiagnosticCaptureCount', 0), 'excluded capture count') == 0 and
            not document.get('error') and not document.get('provenanceError'), 'Report includes excluded/failed diagnostics')
    renderer = resolve(document.get('renderer'), root); renderer_sha = document.get('rendererSha256')
    check_file(renderer, renderer_sha, verified)
    manifest_proof(document.get('nativeManifestBefore' if kind == 'static' else 'manifestBefore'),
        document.get('nativeManifestAfter' if kind == 'static' else 'manifestAfter'), root, renderer, renderer_sha.lower(), verified)
    captures, processes = document.get('captures'), document.get('processes')
    require(isinstance(captures, list) and captures and isinstance(processes, list) and processes, 'Empty report capture/process table')
    expected = document.get('expectedCaptureCount' if kind == 'static' else 'expectedCaptures')
    require(len(captures) == unsigned(expected, 'expected captures'), 'Report has incomplete captures')
    expected_processes = document.get('expectedProcessCount' if kind == 'static' else 'expectedProcesses')
    require(len(processes) == unsigned(expected_processes, 'expected processes'), 'Report has incomplete processes')
    if kind == 'resource':
        require(unsigned(document.get('validatedCaptureCount'), 'validated captures') == len(captures) and
                unsigned(document.get('excludedDiagnosticCaptureCount'), 'excluded captures') == 0,
                'Resource captures were not all validated without exclusions')
    logs = {}
    for process in processes:
        require(type(process.get('returnCode')) is int and process['returnCode'] == 0 and process.get('validationClean') is True and
                process.get('completed' if kind == 'static' else 'validated') is True, 'A native process was not validated')
        log = resolve(process.get('log'), root)
        check_file(log, process.get('logSha256'), verified)
        require(not VALIDATION_ERROR.search(log.read_text(encoding='utf-8', errors='replace')), 'Vulkan/synchronization error in validated log: '+str(log))
        require(str(log) not in logs, 'Duplicated native process log')
        logs[str(log)] = process['logSha256']
        for name, digest in process.get('evidenceFiles', {}).items(): check_file(resolve(name, root), digest, verified)
        if kind == 'resource':
            manifest_proof(process.get('manifestBefore'), process.get('manifestAfter'), root, renderer, renderer_sha.lower(), verified)
    states = {}
    for capture in captures:
        require(capture.get('validationClean' if kind == 'static' else 'validated') is True, 'Capture was excluded/unvalidated')
        image, telemetry, log = (resolve(capture.get(name), root) for name in ('image', 'telemetry', 'log'))
        check_file(image, capture.get('imageSha256'), verified)
        check_file(telemetry, capture.get('telemetrySha256'), verified)
        require(str(log) in logs, 'Capture is not bound to a validated process log')
        if 'logSha256' in capture: require(capture['logSha256'] == logs[str(log)], 'Capture/process log hash differs')
        state = load_json(telemetry); allocation_summary(state); frame_shape(state)
        technique = capture.get('technique')
        require(technique in ('deferred-raster', 'forward-raster') and state.get('technique') == technique, 'Invalid/wrong raster technique')
        label = capture.get('name') if kind == 'static' else (capture.get('case'), unsigned(capture.get('frame'), 'capture frame'))
        require((technique, label) not in states, 'Duplicate capture identity')
        states[technique, label] = state
    # These are immutable inputs/dependencies already recorded by the native
    # harnesses, not new imports of their implementation or image predicates.
    for field in ('sourceDependencies', 'harnessSources', 'generatedInputs'):
        for name, digest in document.get(field, {}).items(): check_file(resolve(name, root), digest, verified)
    for entry in document.get('immutableInputs', []): check_file(resolve(entry['path'], root), entry['sha256'], verified)
    if 'launchPlanSha256' in document: check_file(root/'launch-plan.json', document['launchPlanSha256'], verified)
    return dict(path=str(path), sha256=sha(path), rendererSha256=renderer_sha.lower(), states=states,
        techniques={technique for technique, _ in states}, captureCount=len(captures), processCount=len(processes))


def counter(state, name):
    return unsigned(state['gltfMerge'].get(name), 'gltfMerge.'+name)


def history(state):
    values = state['gltfMaterialVariants']
    return (unsigned(values.get('undoCount'), 'history undoCount'), unsigned(values.get('redoCount'), 'history redoCount'))


def static_checks(report, comparisons, checks):
    for technique in sorted(report['techniques']):
        samples = [report['states'][technique, name] for name in STATIC_STATES]
        baseline = samples[0]
        require(history(baseline) == (3, 0), 'Static two-import history baseline differs')
        for index, state in enumerate(samples[1:4], 1):
            require(state['gltfMerge']['outcome'] == 'failed' and state['gltfMerge']['error'] and
                    counter(state, 'requestId') == counter(baseline, 'requestId')+index and
                    counter(state, 'sourceReadCount') == counter(baseline, 'sourceReadCount')+index,
                    'Failed request did not make exactly one new read attempt')
            require(history(state) == history(baseline) and all(counter(state, name) == counter(baseline, name)
                for name in ('generationPublications', 'commitGeneration', 'descriptorGeneration')), 'Failed request changed history/publication')
            checks.append(dict(name=technique+'/'+STATIC_STATES[index]+'-one-read-no-publication', passed=True))
        final = samples[4]
        require(history(final) == history(baseline) and all(counter(final, name) == counter(baseline, name)+3
            for name in ('requestId', 'sourceReadCount')), 'Final redo lost history or reread an asset')
        require(all(counter(final, name) == counter(baseline, name)+4 for name in
            ('generationPublications', 'commitGeneration', 'descriptorGeneration')), 'Final history replay publication count differs')
        checks.append(dict(name=technique+'/final-redo-preserves-history-and-read-attempts', passed=True))
        for label, state in zip(STATIC_STATES[1:], samples[1:]):
            comparisons.append(compare_allocations(baseline, state, technique+'/redo2-to-'+label, 2))


def resource_checks(report, comparisons, checks):
    for technique in sorted(report['techniques']):
        for case in RESOURCE_CASES:
            merge, redo = (report['states'][technique, (case, frame)] for frame in (26, 52))
            for name in ('requestId', 'sourceReadCount'):
                require(counter(redo, name) == counter(merge, name), 'Resource undo/redo reread a source')
            require(all(counter(redo, name) == counter(merge, name)+2 for name in
                ('generationPublications', 'commitGeneration', 'descriptorGeneration')), 'Resource undo/redo publication count differs')
            require(history(merge) == history(redo) == (1, 0), 'Resource undo/redo history differs')
            comparisons.append(compare_allocations(merge, redo, technique+'/'+case+'/merge-to-redo', 1,
                allow_frame_shape_skip=case == 'frame-rebuild'))
            if case == 'first-extended':
                restored = report['states'][technique, (case, 78)]
                require(history(restored) == (3, 0) and restored['gltfMaterialVariants']['selected'] ==
                    merge['gltfMaterialVariants']['selected'] == -1, 'Extended Default did not restore the same appearance identity')
                require(all(counter(restored, name) == counter(redo, name) for name in
                    ('requestId', 'sourceReadCount', 'generationPublications', 'commitGeneration', 'descriptorGeneration')),
                    'Variant Default restoration reread/replaced an asset generation')
                comparisons.append(compare_allocations(merge, restored, technique+'/'+case+'/merge-to-restored-Default', 1))
            checks.append(dict(name=technique+'/'+case+'-same-retained-assets-no-source-reread', passed=True))


def self_check():
    sample = dict(gltfMerge=dict(vmaAllocationCount=10, vmaAllocationBytes=4096, retainedGenerationCount=2,
        sourceTables=dict(nodes=13, meshes=4)), gltfScenes=dict(graphIdentity=1, selected=0),
        scene=dict(primaryObjectCount=4), resolution=[640, 360], swapchainImages=3)
    compare_allocations(sample, copy.deepcopy(sample), 'synthetic equal settled states', 2)
    rejected = []
    for field, value in (('vmaAllocationCount', 11), ('vmaAllocationBytes', 4097),
            ('vmaAllocationCount', None), ('vmaAllocationBytes', None),
            ('vmaAllocationCount', True), ('vmaAllocationBytes', -1), ('vmaAllocationCount', 1 << 64)):
        changed = copy.deepcopy(sample)
        if value is None: del changed['gltfMerge'][field]
        else: changed['gltfMerge'][field] = value
        try: compare_allocations(sample, changed, 'synthetic invalid VMA witness', 2)
        except RuntimeError: rejected.append((field, value))
        else: raise RuntimeError('Synthetic VMA mutation was accepted')
    resized = copy.deepcopy(sample); resized['swapchainImages'] = 4
    resized['gltfMerge']['vmaAllocationBytes'] += 200
    skip = compare_allocations(sample, resized, 'synthetic changed frame table', 2, allow_frame_shape_skip=True)
    require(not skip['comparisonPerformed'] and 'passed' not in skip, 'Shape mismatch fabricated an equality pass')
    with tempfile.TemporaryDirectory(prefix='gltf-merge-ownership-') as temporary:
        artifact = Path(temporary)/'artifact.bin'; artifact.write_bytes(b'original')
        recorded = sha(artifact); check_file(artifact, recorded, {})
        artifact.write_bytes(b'changed')
        try: check_file(artifact, recorded, {})
        except RuntimeError: rejected.append(('artifactSha256', 'changed'))
        else: raise RuntimeError('Changed input artifact SHA256 was accepted')
    return dict(passed=True, nativeExecuted=False, rejectedMutations=rejected,
        changedFrameShapeExplainsSkippedComparison=True, equalSettledStatesAccepted=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--static-report', type=Path)
    parser.add_argument('--resource-report', type=Path)
    parser.add_argument('--output', type=Path, help='Optional new JSON result; existing reports are never changed')
    parser.add_argument('--self-check', action='store_true')
    args = parser.parse_args()
    if args.self_check:
        print(json.dumps(self_check(), indent=2)); return
    require(args.static_report is not None and args.resource_report is not None, 'Both --static-report and --resource-report are required')
    if args.output: require(not args.output.exists(), 'Refusing to overwrite an existing ownership result')
    verified = {}
    static = checked_report(args.static_report, 'static', verified)
    resource = checked_report(args.resource_report, 'resource', verified)
    require(static['rendererSha256'] == resource['rendererSha256'], 'Static/resource reports used different executables')
    require(static['techniques'] == resource['techniques'], 'Static/resource technique coverage differs')
    comparisons, checks = [], []
    static_checks(static, comparisons, checks); resource_checks(resource, comparisons, checks)
    # Recheck to catch concurrent artifact/source/report replacement during this
    # post-run audit without mutating any original reports or native captures.
    for path, digest in verified.items(): require(sha(Path(path)) == digest, 'Artifact changed during ownership audit: '+path)
    result = dict(schemaVersion=1, passed=True, nativeExecuted=False, rendererSha256=static['rendererSha256'],
        staticReport={key: static[key] for key in ('path', 'sha256', 'captureCount', 'processCount')},
        resourceReport={key: resource[key] for key in ('path', 'sha256', 'captureCount', 'processCount')},
        artifactHashesVerified=len(verified), comparisons=comparisons, counterAndHistoryChecks=checks,
        measuredEqualityCount=sum(row['comparisonPerformed'] for row in comparisons),
        shapeSkippedComparisons=[row for row in comparisons if not row['comparisonPerformed']],
        limitations=['Same-composition settled VMA allocation equality, not driver-object/descriptor-pool/total-memory leak freedom.',
            'Initial borrowed versus later owned buffers, and A states before/after the second retained import, are intentionally excluded.',
            'Ray AS bytes and active DDGI execution remain separately witnessed by the validated resource report.'])
    payload = json.dumps(result, indent=2, allow_nan=False)+'\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open('x', encoding='utf-8') as stream: stream.write(payload)
    print(payload, end='')


if __name__ == '__main__':
    main()
