"""Prepare optional runtime overrides without changing the staged game files."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

UNITY_SETTINGS = Path('app0/Media/globalgamemanagers')
MSAA_OVERRIDE = Path('.anyps5/msaa-off/globalgamemanagers')


def disable_unity_msaa(data, load=None):
    if load is None:
        try:
            import UnityPy
        except ImportError as error:
            raise RuntimeError('Install the runtime settings dependency: python -m pip install UnityPy') from error
        load = UnityPy.load
    environment = load(data)
    objects = [obj for obj in environment.objects if obj.type.name == 'QualitySettings']
    if len(objects) != 1:
        raise ValueError('Expected one Unity QualitySettings object')
    quality = objects[0]
    tree = quality.read_typetree()
    profiles = tree.get('m_QualitySettings')
    if not isinstance(profiles, list) or not profiles:
        raise ValueError('Unity quality profiles are missing')
    for profile in profiles:
        if not isinstance(profile.get('antiAliasing'), int):
            raise ValueError('Unity quality profile has no integer antiAliasing field')
        profile['antiAliasing'] = 0
    quality.save_typetree(tree)
    result = quality.assets_file.save()
    verified = [obj for obj in load(result).objects if obj.type.name == 'QualitySettings']
    if len(verified) != 1 or any(profile['antiAliasing'] != 0
                                for profile in verified[0].read_typetree()['m_QualitySettings']):
        raise ValueError('MSAA override verification failed')
    return result


def prepare_msaa_override(app_dir, transform=disable_unity_msaa):
    app_dir = Path(app_dir).resolve()
    source, target = app_dir / UNITY_SETTINGS, app_dir / MSAA_OVERRIDE
    if not source.is_file():
        raise ValueError('Disable MSAA currently supports Unity games with app0/Media/globalgamemanagers')
    original = source.read_bytes()
    source_hash = hashlib.sha256(original).hexdigest()
    marker = target.with_suffix('.json')
    try:
        stored = json.loads(marker.read_text(encoding='utf-8'))
        if stored.get('version') == 1 and stored.get('source_sha256') == source_hash and target.is_file():
            if stored.get('override_sha256') == hashlib.sha256(target.read_bytes()).hexdigest():
                return target, True
    except (OSError, ValueError, AttributeError):
        pass
    patched = transform(original)
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix('.tmp')
    temporary.write_bytes(patched)
    temporary.replace(target)
    manifest = {'version': 1, 'source_sha256': source_hash,
                'override_sha256': hashlib.sha256(patched).hexdigest()}
    temporary = marker.with_suffix('.tmp')
    temporary.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    temporary.replace(marker)
    return target, False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disable-msaa', action='store_true', required=True)
    parser.add_argument('app_dir', type=Path, help='Converted runtime directory containing app.exe and app0')
    arguments = parser.parse_args()
    try:
        target, reused = prepare_msaa_override(arguments.app_dir)
    except (OSError, ValueError, RuntimeError) as error:
        print(f'Runtime settings: {error}', file=sys.stderr)
        return 1
    print(f'MSAA disabled: {"reused" if reused else "prepared"} {target}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
