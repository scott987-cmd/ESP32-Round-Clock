import copy
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import companion_apps as apps


def board(machine='mac-a', state='running'):
    return {'machineID': machine, 'nodeName': 'Mac mini', 'generatedAt': '2026-09-15T03:00:00Z',
            'tasks': [{'id': 'same-id', 'title': '检查屏幕', 'state': state, 'platform': 'codex',
                       'progressSummary': '正在检查', 'stepIndex': 2, 'stepTotal': 5}]}


class AgentTests(unittest.TestCase):
    def test_machine_identity_prevents_task_collision(self):
        result = apps.agent_snapshot({'boards': [board(), board('mac-b')]}, now=apps.timestamp('2026-09-15T03:01:00Z'))
        self.assertEqual(len({t['id'] for t in result['tasks']}), 2)
        self.assertEqual(result['counts']['running'], 2)

    def test_stale_running_is_not_live(self):
        result = apps.agent_snapshot({'boards': [board()]}, now=apps.timestamp('2026-09-15T04:00:00Z'))
        self.assertEqual(result['counts']['running'], 0)
        self.assertEqual(result['tasks'][0]['stateLabel'], '状态已过期')

    def test_missing_time_and_future_time_are_not_live(self):
        for date in (None, 'invalid', '2027-01-01T00:00:00Z'):
            b = board(); b['generatedAt'] = date
            result = apps.agent_snapshot({'boards': [b]}, now=apps.timestamp('2026-09-15T03:00:00Z'))
            self.assertTrue(result['tasks'][0]['stale'])

    def test_done_is_not_a_claim_of_merge(self):
        result = apps.agent_snapshot({'boards': [board(state='done')]})
        self.assertEqual(result['tasks'][0]['stateLabel'], '已结束')

    def test_failed_terminal_is_not_running_or_success(self):
        result = apps.agent_snapshot({'boards': [board(state='failed')]})
        self.assertEqual(result['tasks'][0]['stateLabel'], '未完成')

    def test_no_artificial_progress_percentage(self):
        b=board(); b['tasks'][0].pop('stepTotal')
        result=apps.agent_snapshot({'boards': [b]})
        self.assertEqual(result['tasks'][0]['progress'], '')

    def test_delayed_snapshot_does_not_replace_newer_board(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(apps, 'AGENT_FILE', Path(folder)/'boards.json'):
            fresh=board(state='done')
            apps.store_boards({'boards': [fresh]})
            old=board(); old['generatedAt']='2026-09-14T03:00:00Z'
            apps.store_boards({'boards': [old]})
            self.assertEqual(apps.agent_snapshot()['tasks'][0]['state'], 'done')

    def test_missing_file_does_not_mean_zero_tasks(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(apps, 'AGENT_FILE', Path(folder)/'missing.json'):
            self.assertFalse(apps.agent_snapshot()['ready'])

    def test_reject_duplicate_board_or_task(self):
        with self.assertRaises(ValueError): apps.validate_boards({'boards': [board(), board()]})
        b=board(); b['tasks'].append(copy.deepcopy(b['tasks'][0]))
        with self.assertRaises(ValueError): apps.validate_boards({'boards': [b]})


class NoteTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.db=patch.object(apps,'NOTES_DB',Path(self.temp.name)/'notes.sqlite3')
        self.ready=patch.object(apps,'NOTE_READY',True)
        self.db.start();self.ready.start()
        self.note={'id':'a'*32,'text':'明天下午检查屏幕，不要发布。'}

    def tearDown(self):
        self.ready.stop();self.db.stop();self.temp.cleanup()

    def test_raw_text_is_durable_before_generation(self):
        apps.create_note(self.note,enqueue=False)
        result=apps.list_notes()['notes'][0]
        self.assertEqual(result['rawText'],self.note['text'])
        self.assertEqual(result['state'],'pending')

    def test_same_id_retries_are_idempotent(self):
        apps.create_note(self.note,enqueue=False)
        apps.create_note(self.note,enqueue=False)
        self.assertEqual(apps.list_notes()['total'],1)

    def test_same_id_cannot_overwrite_other_content(self):
        apps.create_note(self.note,enqueue=False)
        with self.assertRaises(ValueError):
            apps.create_note({**self.note,'text':'不同的内容'},enqueue=False)
        self.assertEqual(apps.list_notes()['notes'][0]['rawText'],self.note['text'])

    def test_model_failure_keeps_original_and_allows_retry(self):
        apps.create_note(self.note,enqueue=False)
        with patch.object(apps,'summarize_note',side_effect=TimeoutError): apps.process_note(self.note['id'])
        result=apps.list_notes()['notes'][0]
        self.assertEqual(result['state'],'saved');self.assertEqual(result['rawText'],self.note['text'])
        apps.create_note({'id':self.note['id'],'retry':True},enqueue=False)
        self.assertEqual(apps.list_notes()['notes'][0]['state'],'pending')

    def test_success_keeps_raw_and_parses_todos(self):
        apps.create_note(self.note,enqueue=False)
        summary={'title':'检查屏幕','summary':'明天下午检查屏幕，暂不发布。','todos':['明天下午检查屏幕']}
        with patch.object(apps,'summarize_note',return_value=summary) as model:
            apps.process_note(self.note['id']); apps.process_note(self.note['id'])
            self.assertEqual(model.call_count,1)
        result=apps.list_notes()['notes'][0]
        self.assertEqual(result['state'],'ready');self.assertEqual(result['rawText'],self.note['text'])
        self.assertEqual(result['todos'],summary['todos'])

    def test_invalid_input_rejected(self):
        for value in ({}, {'id':'../escape','text':'x'}, {'id':'a'*32,'text':''}, {'id':'a'*32,'text':'汉'*1000}, []):
            with self.assertRaises(ValueError): apps.create_note(value,enqueue=False)

    def test_paging_preserves_older_notes(self):
        for i in range(15): apps.create_note({'id':f'{i:032x}','text':str(i)},enqueue=False)
        first=apps.list_notes(); second=apps.list_notes(first['nextOffset'])
        self.assertEqual(len(first['notes']),12);self.assertEqual(len(second['notes']),3)
        self.assertEqual(len({n['id'] for n in first['notes']+second['notes']}),15)

    def test_concurrent_same_request_stores_once(self):
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(max_workers=4) as executor:
            list(executor.map(lambda _: apps.create_note(self.note,enqueue=False),range(8)))
        self.assertEqual(apps.list_notes()['total'],1)


if __name__ == '__main__': unittest.main()
